#include <Arduino.h>
#include <Preferences.h>
#include <WiFi.h>
#include <driver/gpio.h>
#include <esp_http_server.h>
#include <esp_random.h>
#include <esp_system.h>
#include <lwip/sockets.h>
#include <mbedtls/platform_util.h>

#include "config.h"
#include "controller.h"
#include "eduroam.h"
#include "hall_feedback.h"
#include "speed_control.h"

namespace {
constexpr uint32_t kPwmHz = 490;
constexpr uint8_t kPwmBits = 8;
constexpr int kMaxDuty = (1 << kPwmBits) - 1;
constexpr uint32_t kCommandTimeoutMs = 350;
constexpr uint32_t kDirectionPauseMs = 100;
constexpr uint32_t kDirectionPauseWithoutHallMs = 1500;
constexpr uint32_t kUpdatePeriodMs = 10;
constexpr int kDutyStep = 3;

constexpr uint32_t kFeedbackTimeoutMs = 1500;
constexpr float kMinWheelRpm = 3;
constexpr float kDirectionChangeMaxRpm = 50;
constexpr unsigned kMaxPulsesPerRev = 1000;
constexpr int kBenchMaxRpm = 40;
constexpr unsigned kBenchMaxMs = 4000;
constexpr float kBenchDutyLimit = 0.35f;

constexpr bool distinctPins() {
  constexpr int pins[] = {kLeftMotor.pwm, kLeftMotor.direction, kLeftMotor.hall,
                          kRightMotor.pwm, kRightMotor.direction, kRightMotor.hall};
  for (size_t i = 0; i < std::size(pins); ++i) {
    for (size_t j = i + 1; j < std::size(pins); ++j) {
      if (pins[i] == pins[j]) return false;
    }
  }
  return true;
}
static_assert(distinctPins(), "Motor and Hall GPIOs must be distinct");

enum DriveFault : unsigned { NoFault = 0, LeftHall = 1, RightHall = 2, Pwm = 4 };
enum class WheelResult { Ok, FeedbackLost, PwmError };

class Wheel {
 public:
  explicit Wheel(MotorPins pins) : pins_(pins) {}

  WheelTune tune;
  HallReading hall() { return feedback_.read(); }
  // Hall C measures speed magnitude. Use the active direction output for telemetry.
  float rpm() const { return forward_ ? measuredRpm_ : -measuredRpm_; }
  int duty() const { return duty_; }
  PidTerms pidTerms() const { return pid_.terms(); }
  void resetPid() { pid_.reset(measuredRpm_); }

  bool begin() {
    pinMode(pins_.pwm, OUTPUT);
    digitalWrite(pins_.pwm, LOW);
    pinMode(pins_.direction, OUTPUT);
    digitalWrite(pins_.direction, pins_.forwardLevel);
    feedback_.begin(pins_.hall);
    return ledcAttach(pins_.pwm, kPwmHz, kPwmBits) && ledcWrite(pins_.pwm, 0);
  }

  void disable() {
    ledcDetach(pins_.pwm);
    pinMode(pins_.pwm, OUTPUT);
    digitalWrite(pins_.pwm, LOW);
  }

  WheelResult update(float target, uint32_t now, int64_t nowUs, float seconds, bool bypassHall, float dutyLimit = kMaxDutyFraction) {
    const HallReading pulses = hall();
    const float rawRpm = estimateRpm(pulses, tune.pulsesPerRev, nowUs);
    measuredRpm_ += (rawRpm - measuredRpm_) * seconds / (0.08f + seconds);
    if (std::abs(target) < kMinWheelRpm) return coast(now);

    const bool forward = target > 0;
    if (forward != forward_) {
      const WheelResult stopped = coast(now);
      if (stopped != WheelResult::Ok) return stopped;
      const uint32_t pause = bypassHall ? kDirectionPauseWithoutHallMs : kDirectionPauseMs;
      const float maxChangeRpm = bypassHall ? kMinWheelRpm : kDirectionChangeMaxRpm;
      if ((hasDriven_ && now - lastPoweredAt_ < pause) || rawRpm > maxChangeRpm) {
        return WheelResult::Ok;
      }
      digitalWrite(pins_.direction, forward ? pins_.forwardLevel : !pins_.forwardLevel);
      forward_ = forward;
      return WheelResult::Ok;
    }

    if (bypassHall) resetPid();
    const float speed = std::abs(target);
    const float fraction = bypassHall
      ? kHallOverrideDuty * std::min(speed / kHallOverrideRpm, 1.f)
      : pid_.update(tune, speed, measuredRpm_, seconds, dutyLimit);
    const int desired = bypassHall
      ? static_cast<int>(fraction * kMaxDuty)
      : std::lround(fraction * kMaxDuty);
    const int next = constrain(desired, duty_ - kDutyStep, duty_ + kDutyStep);
    if (!writeDuty(next, now)) return WheelResult::PwmError;
    // A nonzero setpoint can still produce zero duty while the wheel coasts.
    if (next == 0 || bypassHall) {
      watchingFeedback_ = false;
    } else if (!watchingFeedback_ || pulses.pulses != priorPulses_) {
      watchingFeedback_ = true;
      priorPulses_ = pulses.pulses;
      feedbackAt_ = now;
    } else if (now - feedbackAt_ >= kFeedbackTimeoutMs) {
      return WheelResult::FeedbackLost;
    }
    return WheelResult::Ok;
  }

  WheelResult coast(uint32_t now) {
    resetPid();
    watchingFeedback_ = false;
    return writeDuty(0, now) ? WheelResult::Ok : WheelResult::PwmError;
  }

 private:
  bool writeDuty(int next, uint32_t now) {
    if (duty_ > 0) {
      hasDriven_ = true;
      lastPoweredAt_ = now;
    }
    if (next == duty_) return true;
    if (!ledcWrite(pins_.pwm, next)) return false;
    duty_ = next;
    return true;
  }

  MotorPins pins_;
  HallFeedback feedback_;
  SpeedPid pid_;
  float measuredRpm_ = 0;
  bool watchingFeedback_ = false;
  uint32_t priorPulses_ = 0;
  uint32_t feedbackAt_ = 0;
  bool forward_ = true;
  bool hasDriven_ = false;
  int duty_ = 0;
  uint32_t lastPoweredAt_ = 0;
};

struct Command {
  WheelSpeeds targets;
  uint32_t receivedAt = 0;
  bool moving() const { return targets.left != 0 || targets.right != 0; }
};

// The HTTP server runs in its own task. Only the Arduino loop touches motor outputs.
portMUX_TYPE controlLock = portMUX_INITIALIZER_UNLOCKED;
Wheel leftWheel{kLeftMotor};
Wheel rightWheel{kRightMotor};
bool motorsReady = false;
bool calibrated = false;
unsigned fault = NoFault;
bool hallOverride = false;
WheelSpeeds measured;
enum class Telemetry { Off, Basic, Pid };
Telemetry telemetry = Telemetry::Off;
uint32_t session = 0;
uint32_t expiredSession = 0;
uint32_t lastSequence = 0;
uint32_t bootId = 0;
bool bootedAfterBrownout = false;
Command command;

struct WheelSample {
  float rpm;
  int duty;
  PidTerms terms;
  WheelTune tune;
};
struct DriveSample {
  uint32_t ms;
  WheelSpeeds targets;
  WheelSample left;
  WheelSample right;
};
DriveSample sample{};

struct TuneRequest {
  uint32_t id;
  char side;
  WheelTune gains;
};
TuneRequest tuning{};
uint32_t tuneSequence = 0;
uint32_t finishedTune = 0;
bool tuneSucceeded = false;

struct BenchRun {
  uint32_t token = 0;
  uint32_t startedAt = 0;
  uint32_t durationMs = 0;
  bool active() const { return token != 0; }
};
BenchRun bench;
const char* benchEnd = nullptr;

// Call while holding controlLock. The main loop reports the result over USB.
void finishBench(const char* reason) {
  bench = {};
  command = {};
  benchEnd = reason;
}

void expireCommand(uint32_t now) {
  if (bench.active()) {
    if (now - command.receivedAt >= kCommandTimeoutMs) finishBench("link-timeout");
    else if (now - bench.startedAt >= bench.durationMs) finishBench("done");
    return;
  }
  if (command.moving() && now - command.receivedAt >= kCommandTimeoutMs) {
    expiredSession = session;
    session = 0;
    hallOverride = false;
    command = {};
  }
}

bool validPin(int pin) {
  return pin >= 0 && GPIO_IS_VALID_OUTPUT_GPIO(pin);
}

esp_err_t reply(httpd_req_t* request, const char* status, const char* body = "") {
  httpd_resp_set_status(request, status);
  httpd_resp_set_hdr(request, "Cache-Control", "no-store");
  return httpd_resp_sendstr(request, body);
}

bool controllerRequest(httpd_req_t* request) {
  // A cross-origin browser cannot send this header without a successful CORS preflight.
  char header[4];
  return httpd_req_get_hdr_value_str(request, "X-Drive", header, sizeof(header)) == ESP_OK &&
         strcmp(header, "1") == 0;
}

esp_err_t servePage(httpd_req_t* request) {
  httpd_resp_set_type(request, "text/html; charset=utf-8");
  return reply(request, "200 OK", kControllerPage);
}

bool readyToDrive() {
  return motorsReady && calibrated && (fault == NoFault || hallOverride);
}

esp_err_t replyStatus(httpd_req_t* request, uint32_t token = 0, const char* rejection = nullptr,
                      bool websocket = false) {
  portENTER_CRITICAL(&controlLock);
  const bool benchActive = bench.active();
  const bool ready = readyToDrive() && !benchActive && !tuning.id;
  const unsigned faultCode = fault;
  const bool overrideActive = hallOverride;
  const bool needsCalibration = motorsReady && !calibrated;
  const DriveSample snapshot = sample;
  const bool tuneBusy = tuning.id != 0;
  const uint32_t tuneId = finishedTune;
  const bool tuneOk = tuneSucceeded;
  portEXIT_CRITICAL(&controlLock);

  char json[768];
  snprintf(json, sizeof(json),
           "{\"session\":%lu,\"boot\":%lu,\"brownout\":%s,\"rejection\":\"%s\","
           "\"ready\":%s,\"calibrate\":%s,\"fault\":%u,\"override\":%s,\"bench\":%s,"
           "\"max\":%d,\"ms\":%u,\"left\":%.1f,\"right\":%.1f,\"leftTarget\":%.1f,\"rightTarget\":%.1f,"
           "\"leftPid\":[%.3f,%.3f,%.3f,%.1f],\"rightPid\":[%.3f,%.3f,%.3f,%.1f],"
           "\"leftGains\":[%.8g,%.8g,%.8g],\"rightGains\":[%.8g,%.8g,%.8g],"
           "\"tuneBusy\":%s,\"tuneId\":%u,\"tuneOk\":%s}",
           static_cast<unsigned long>(token), static_cast<unsigned long>(bootId),
           bootedAfterBrownout ? "true" : "false", rejection ? rejection : "", ready ? "true" : "false",
           needsCalibration ? "true" : "false", faultCode, overrideActive ? "true" : "false", benchActive ? "true" : "false",
           overrideActive ? kHallOverrideRpm : kMaxWheelRpm,
           snapshot.ms, snapshot.left.rpm, snapshot.right.rpm, snapshot.targets.left, snapshot.targets.right,
           100.f * snapshot.left.terms.p, 100.f * snapshot.left.terms.i, 100.f * snapshot.left.terms.d,
           100.f * snapshot.left.duty / kMaxDuty,
           100.f * snapshot.right.terms.p, 100.f * snapshot.right.terms.i, 100.f * snapshot.right.terms.d,
           100.f * snapshot.right.duty / kMaxDuty,
           snapshot.left.tune.kp, snapshot.left.tune.ki, snapshot.left.tune.kd,
           snapshot.right.tune.kp, snapshot.right.tune.ki, snapshot.right.tune.kd,
           tuneBusy ? "true" : "false", tuneId, tuneOk ? "true" : "false");
  if (websocket) {
    httpd_ws_frame_t frame{};
    frame.type = HTTPD_WS_TYPE_TEXT;
    frame.payload = reinterpret_cast<uint8_t*>(json);
    frame.len = strlen(json);
    return httpd_ws_send_frame(request, &frame);
  }
  httpd_resp_set_type(request, "application/json");
  return reply(request, rejection ? "409 Conflict" : "200 OK", json);
}

esp_err_t openControl(httpd_req_t* request) {
  if (!controllerRequest(request)) return reply(request, "403 Forbidden");
  const uint32_t token = esp_random() | 1U;
  portENTER_CRITICAL(&controlLock);
  session = token;
  lastSequence = 0;
  hallOverride = false;
  if (!bench.active()) command = {};
  portEXIT_CRITICAL(&controlLock);
  return replyStatus(request, token);
}

esp_err_t getStatus(httpd_req_t* request) {
  return replyStatus(request);
}

bool readNumber(const char* query, const char* key, uint32_t& result) {
  char value[11];
  if (httpd_query_key_value(query, key, value, sizeof(value)) != ESP_OK || !value[0]) return false;
  result = 0;
  for (const char* digit = value; *digit; ++digit) {
    if (*digit < '0' || *digit > '9' || result > (UINT32_MAX - (*digit - '0')) / 10U) return false;
    result = result * 10U + (*digit - '0');
  }
  return true;
}

const char* applyDrive(const char* query) {
  uint32_t id, sequence, x, y, speed, action;
  if (!readNumber(query, "session", id) || id == 0 ||
      !readNumber(query, "seq", sequence) || sequence == 0 ||
      !readNumber(query, "x", x) || x > 200 || !readNumber(query, "y", y) || y > 200 ||
      !readNumber(query, "rpm", speed) || speed > kMaxWheelRpm ||
      !readNumber(query, "clear", action) || action > 2 || (action && (x != 100 || y != 100))) {
    return "format";
  }

  Command next;
  // The wheel polarities set chassis forward. Reverse the turn mix to keep the same steering.
  next.targets = mixSpeeds(100 - static_cast<int>(x), static_cast<int>(y) - 100, speed);
  char mode[2];
  const esp_err_t modeResult = httpd_query_key_value(query, "direct", mode, sizeof(mode));
  if (modeResult != ESP_ERR_NOT_FOUND) {
    uint32_t left, right;
    if (modeResult != ESP_OK || strcmp(mode, "1") || action || x != 100 || y != 100 ||
        !readNumber(query, "left", left) || left > 2 * kMaxWheelRpm ||
        !readNumber(query, "right", right) || right > 2 * kMaxWheelRpm) {
      return "format";
    }
    // The direct targets are sent as unsigned RPM offset by the configured maximum.
    next.targets = {static_cast<float>(static_cast<int>(left) - kMaxWheelRpm),
                    static_cast<float>(static_cast<int>(right) - kMaxWheelRpm)};
  }
  if (std::abs(next.targets.left) < kMinWheelRpm) next.targets.left = 0;
  if (std::abs(next.targets.right) < kMinWheelRpm) next.targets.right = 0;

  portENTER_CRITICAL(&controlLock);
  next.receivedAt = millis();
  expireCommand(next.receivedAt);
  const char* rejection = nullptr;
  if (session != id) rejection = expiredSession == id ? "expired" : "session";
  else if (sequence <= lastSequence) rejection = "sequence";
  else if ((bench.active() && action != 1) ||
           (next.moving() && (!readyToDrive() || tuning.id)) ||
           (action == 2 && (!motorsReady || !calibrated || !(fault & (LeftHall | RightHall))))) {
    rejection = "blocked";
  }
  if (!rejection) {
    lastSequence = sequence;
    if (bench.active()) finishBench("web-stop");
    command = next;
    if (action == 2) {
      hallOverride = true;
    } else if (action == 1) {
      if (!hallOverride && fault != Pwm) fault = NoFault;
      hallOverride = false;
    }
    if (hallOverride && next.moving()) {
      const float peak = std::max(std::abs(next.targets.left), std::abs(next.targets.right));
      if (peak > kHallOverrideRpm) {
        command.targets.left *= kHallOverrideRpm / peak;
        command.targets.right *= kHallOverrideRpm / peak;
      }
    }
  }
  portEXIT_CRITICAL(&controlLock);

  return rejection;
}

esp_err_t drive(httpd_req_t* request) {
  if (!controllerRequest(request)) return reply(request, "403 Forbidden");
  char query[128];
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK) return reply(request, "400 Bad Request");
  const char* rejection = applyDrive(query);
  if (rejection && !strcmp(rejection, "format")) return reply(request, "400 Bad Request");
  return replyStatus(request, 0, rejection);
}

esp_err_t driveSocket(httpd_req_t* request) {
  // Upgrading opens the transport only. Each frame must carry a valid control session.
  if (request->method == HTTP_GET) return ESP_OK;
  char command[128];
  httpd_ws_frame_t frame{};
  if (httpd_ws_recv_frame(request, &frame, 0) != ESP_OK ||
      frame.type != HTTPD_WS_TYPE_TEXT || frame.fragmented || frame.len >= sizeof(command)) return ESP_FAIL;
  frame.payload = reinterpret_cast<uint8_t*>(command);
  if (httpd_ws_recv_frame(request, &frame, sizeof(command) - 1) != ESP_OK) return ESP_FAIL;
  if (memchr(command, '\0', frame.len)) return ESP_FAIL;
  command[frame.len] = '\0';
  return replyStatus(request, 0, applyDrive(command), true);
}

bool validGains(const WheelTune& tune);

bool readGain(const char* query, const char* key, float& gain) {
  char text[24];
  if (httpd_query_key_value(query, key, text, sizeof(text)) != ESP_OK || !text[0]) return false;
  char* end;
  gain = strtof(text, &end);
  return end != text && !*end;
}

esp_err_t tuneDrive(httpd_req_t* request) {
  if (!controllerRequest(request)) return reply(request, "403 Forbidden");
  char query[160], side[2];
  uint32_t token;
  TuneRequest next{};
  if (httpd_req_get_url_query_str(request, query, sizeof(query)) != ESP_OK ||
      !readNumber(query, "session", token) || !token ||
      httpd_query_key_value(query, "side", side, sizeof(side)) != ESP_OK) {
    return reply(request, "400 Bad Request");
  }
  next.side = side[0];
  // 's' saves both wheels. 'l' and 'r' apply gains to the selected wheel.
  if (next.side != 's' &&
      ((next.side != 'l' && next.side != 'r') ||
       !readGain(query, "kp", next.gains.kp) || !readGain(query, "ki", next.gains.ki) ||
       !readGain(query, "kd", next.gains.kd) || !validGains(next.gains))) {
    return reply(request, "400 Bad Request");
  }

  portENTER_CRITICAL(&controlLock);
  if (session == token && !bench.active() && !command.moving() && !tuning.id &&
      !sample.left.duty && !sample.right.duty) {
    if (++tuneSequence == 0) ++tuneSequence;
    next.id = tuneSequence;
    tuning = next;
  }
  portEXIT_CRITICAL(&controlLock);
  if (!next.id) return reply(request, "409 Conflict");

  char json[32];
  snprintf(json, sizeof(json), "{\"id\":%u}", next.id);
  httpd_resp_set_type(request, "application/json");
  return reply(request, "202 Accepted", json);
}

uint32_t ipv4Address(const sockaddr_storage& address) {
  if (address.ss_family == AF_INET) {
    return reinterpret_cast<const sockaddr_in&>(address).sin_addr.s_addr;
  }
  if (address.ss_family == AF_INET6) {
    const auto& ipv6 = reinterpret_cast<const sockaddr_in6&>(address).sin6_addr;
    if (IN6_IS_ADDR_V4MAPPED(&ipv6)) {
      uint32_t value;
      memcpy(&value, &ipv6.s6_addr[12], sizeof(value));
      return value;
    }
  }
  return 0;
}

esp_err_t allowDriveClient(httpd_handle_t, int socket) {
  sockaddr_storage local{};
  socklen_t localSize = sizeof(local);
  if (getsockname(socket, reinterpret_cast<sockaddr*>(&local), &localSize)) return ESP_FAIL;
  const uint32_t destination = ipv4Address(local);
  if (!destination) return ESP_FAIL;

  const uint32_t station = WiFi.localIP();
  if (station == destination && WiFi.status() == WL_CONNECTED) return ESP_OK;

  const uint32_t hotspot = WiFi.softAPIP();
  const uint32_t mask = WiFi.AP.subnetMask();
  if (!mask || destination != hotspot) return ESP_FAIL;
  sockaddr_storage peer{};
  socklen_t peerSize = sizeof(peer);
  if (getpeername(socket, reinterpret_cast<sockaddr*>(&peer), &peerSize)) return ESP_FAIL;
  const uint32_t client = ipv4Address(peer);
  return client && (client & mask) == (hotspot & mask) ? ESP_OK : ESP_FAIL;
}

bool startServer() {
  httpd_config_t config = HTTPD_DEFAULT_CONFIG();
  config.lru_purge_enable = true;
  config.recv_wait_timeout = 1;
  config.send_wait_timeout = 1;
  config.open_fn = allowDriveClient;
  httpd_handle_t server = nullptr;
  if (httpd_start(&server, &config) != ESP_OK) return false;

  const httpd_uri_t routes[] = {
    {.uri = "/", .method = HTTP_GET, .handler = servePage},
    {.uri = "/control", .method = HTTP_POST, .handler = openControl},
    {.uri = "/drive", .method = HTTP_POST, .handler = drive},
    {.uri = "/motion", .method = HTTP_GET, .handler = driveSocket, .is_websocket = true},
    {.uri = "/status", .method = HTTP_GET, .handler = getStatus},
    {.uri = "/tune", .method = HTTP_POST, .handler = tuneDrive},
  };
  for (const auto& route : routes) {
    if (httpd_register_uri_handler(server, &route) != ESP_OK) {
      httpd_stop(server);
      return false;
    }
  }
  return true;
}
struct StoredTuning {
  int leftHall;
  int rightHall;
  WheelTune left;
  WheelTune right;
};

bool validGains(const WheelTune& tune) {
  return tune.kp >= 0 && tune.kp <= 1 && tune.ki >= 0 && tune.ki <= 2 &&
         tune.kd >= -1 && tune.kd <= 1;
}

void loadTuning() {
  Preferences nvs;
  StoredTuning stored{};
  if (nvs.begin("wheel-speed", true) &&
      nvs.getBytesLength("tune1") == sizeof(stored) &&
      nvs.getBytes("tune1", &stored, sizeof(stored)) == sizeof(stored) &&
      stored.leftHall == kLeftMotor.hall && stored.rightHall == kRightMotor.hall &&
      stored.left.pulsesPerRev <= kMaxPulsesPerRev && stored.right.pulsesPerRev <= kMaxPulsesPerRev &&
      validGains(stored.left) && validGains(stored.right)) {
    leftWheel.tune = stored.left;
    rightWheel.tune = stored.right;
  }
}

bool saveTuning() {
  Preferences nvs;
  const StoredTuning stored{kLeftMotor.hall, kRightMotor.hall, leftWheel.tune, rightWheel.tune};
  return nvs.begin("wheel-speed", false) &&
         nvs.putBytes("tune1", &stored, sizeof(stored)) == sizeof(stored);
}

void applyGains(Wheel& wheel, WheelTune gains) {
  gains.pulsesPerRev = wheel.tune.pulsesPerRev;
  wheel.tune = gains;
  wheel.resetPid();
}

void showWheel(char side, Wheel& wheel) {
  const WheelTune& tune = wheel.tune;
  Serial.printf("%c: pulses=%lu ppr=%u Kp=%.5f Ki=%.5f Kd=%.5f\n", side,
                static_cast<unsigned long>(wheel.hall().pulses), tune.pulsesPerRev,
                tune.kp, tune.ki, tune.kd);
}

void handleBenchCommand(const char* line) {
  if (!strcmp(line, "status")) {
    portENTER_CRITICAL(&controlLock);
    const bool active = bench.active();
    const bool ready = readyToDrive() && fault == NoFault && !command.moving();
    const unsigned currentFault = fault;
    portEXIT_CRITICAL(&controlLock);
    Serial.printf("BENCH STATUS ready=%d fault=%u active=%d log=%d\n",
                  ready, currentFault, active, telemetry != Telemetry::Off);
    return;
  }
  if (!strcmp(line, "stop")) {
    portENTER_CRITICAL(&controlLock);
    if (bench.active()) finishBench("usb-stop");
    portEXIT_CRITICAL(&controlLock);
    return;
  }

  char extra;
  unsigned token;
  if (sscanf(line, "keep %u %c", &token, &extra) == 1) {
    portENTER_CRITICAL(&controlLock);
    const uint32_t now = millis();
    expireCommand(now);
    if (bench.active() && bench.token == token) command.receivedAt = now;
    portEXIT_CRITICAL(&controlLock);
    return;
  }

  char side;
  int rpm;
  unsigned duration;
  if (sscanf(line, "start %c %d %u %c", &side, &rpm, &duration, &extra) != 3 ||
      (side != 'l' && side != 'r') || rpm < 10 || rpm > kBenchMaxRpm ||
      duration < 500 || duration > kBenchMaxMs) {
    Serial.println("BENCH REJECT invalid-command");
    return;
  }

  token = esp_random() | 1U;
  portENTER_CRITICAL(&controlLock);
  const bool accepted = motorsReady && calibrated && fault == NoFault && !command.moving() &&
                        !bench.active() && !benchEnd && !tuning.id && leftWheel.duty() == 0 && rightWheel.duty() == 0 &&
                        std::abs(measured.left) < kMinWheelRpm && std::abs(measured.right) < kMinWheelRpm;
  if (accepted) {
    const uint32_t now = millis();
    bench = {token, now, duration};
    hallOverride = false;
    const float speed = rpm;
    command = {{side == 'l' ? speed : 0.f, side == 'r' ? speed : 0.f}, now};
  }
  portEXIT_CRITICAL(&controlLock);

  if (accepted) Serial.printf("BENCH START %u\n", token);
  else Serial.println("BENCH REJECT drive-busy-or-faulted");
}

void handleSerialCommand(char* line, bool idle) {
  if (!strncmp(line, "bench ", 6)) {
    handleBenchCommand(line + 6);
    return;
  }
  if (!strcmp(line, "show")) {
    showWheel('l', leftWheel);
    showWheel('r', rightWheel);
    return;
  }
  if (!strcmp(line, "log on")) {
    telemetry = Telemetry::Basic;
    Serial.println("ms,l_target_rpm,l_measured_rpm,l_pwm,r_target_rpm,r_measured_rpm,r_pwm");
    return;
  }
  if (!strcmp(line, "log off")) {
    telemetry = Telemetry::Off;
    return;
  }
  if (!strcmp(line, "log pid")) {
    telemetry = Telemetry::Pid;
    return;
  }
  if (!strcmp(line, "log status")) {
    Serial.printf("LOG %s\n", telemetry == Telemetry::Pid ? "pid" :
                             telemetry == Telemetry::Basic ? "basic" : "off");
    return;
  }
  if (!strcmp(line, "help")) {
    Serial.println("show | ppr l|r <pulses> | pid l|r <Kp> <Ki> <Kd> | save | log on | log pid | log off | bench status");
    Serial.println("eduroam status | eduroam forget");
    return;
  }
  portENTER_CRITICAL(&controlLock);
  const bool driveIdle = !command.moving() && !tuning.id;
  portEXIT_CRITICAL(&controlLock);
  if (!strncmp(line, "eduroam ", 8)) {
    handleEduroamCommand(line + 8, idle && driveIdle && !leftWheel.duty() && !rightWheel.duty());
    return;
  }
  if (!idle || !driveIdle) {
    Serial.println("Release the joystick before changing tuning.");
    return;
  }

  char side = 0;
  char extra = 0;
  unsigned ppr;
  WheelTune gains;
  if (!strcmp(line, "save")) {
    Serial.println(saveTuning() ? "Tuning saved." : "Could not save tuning.");
  } else if (sscanf(line, "ppr %c %u %c", &side, &ppr, &extra) == 2 &&
             (side == 'l' || side == 'r') && ppr > 0 && ppr <= kMaxPulsesPerRev) {
    Wheel& wheel = side == 'l' ? leftWheel : rightWheel;
    wheel.tune.pulsesPerRev = ppr;
    wheel.resetPid();
    showWheel(side, wheel);
  } else if (sscanf(line, "pid %c %f %f %f %c", &side, &gains.kp, &gains.ki,
                    &gains.kd, &extra) == 4 && (side == 'l' || side == 'r') && validGains(gains)) {
    Wheel& wheel = side == 'l' ? leftWheel : rightWheel;
    applyGains(wheel, gains);
    showWheel(side, wheel);
  } else {
    Serial.println("Invalid command or value. Type help.");
  }
}

void serviceSerial(bool idle, uint32_t now, WheelSpeeds targets) {
  static char input[256];
  static size_t length = 0;
  static bool overflow = false;
  for (int count = 0; count < 64 && Serial.available(); ++count) {
    const int byte = Serial.read();
    if (byte == '\r' || byte == '\n') {
      if (overflow) {
        Serial.println("Serial command too long.");
      } else if (length) {
        input[length] = '\0';
        handleSerialCommand(input, idle);
      }
      if (length) mbedtls_platform_zeroize(input, length);
      length = 0;
      overflow = false;
    } else if (length < sizeof(input) - 1) {
      input[length++] = byte;
    } else {
      overflow = true;
    }
  }

  static uint32_t lastLogAt = 0;
  if (telemetry == Telemetry::Off || now - lastLogAt < 100 || Serial.availableForWrite() < 180) return;
  lastLogAt = now;
  if (telemetry == Telemetry::Pid) {
    const auto left = leftWheel.pidTerms();
    const auto right = rightWheel.pidTerms();
    Serial.printf("PID,%lu,%.1f,%.1f,%d,%.1f,%.1f,%d,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f\n",
                  static_cast<unsigned long>(now), targets.left, leftWheel.rpm(), leftWheel.duty(),
                  targets.right, rightWheel.rpm(), rightWheel.duty(),
                  100.f * left.p, 100.f * left.i, 100.f * left.d,
                  100.f * right.p, 100.f * right.i, 100.f * right.d);
  } else {
    Serial.printf("%lu,%.1f,%.1f,%d,%.1f,%.1f,%d\n", static_cast<unsigned long>(now),
                  targets.left, leftWheel.rpm(), leftWheel.duty(),
                  targets.right, rightWheel.rpm(), rightWheel.duty());
  }
}
}  // namespace

void setup() {
  Serial.setTxBufferSize(512);
  Serial.begin(115200);
  bootedAfterBrownout = esp_reset_reason() == ESP_RST_BROWNOUT;
  if (bootedAfterBrownout) Serial.println("ESP32 reset after a brownout.");
  loadTuning();
  calibrated = leftWheel.tune.pulsesPerRev && rightWheel.tune.pulsesPerRev;

  if (validPin(kLeftMotor.pwm) && validPin(kLeftMotor.direction) &&
      validPin(kRightMotor.pwm) && validPin(kRightMotor.direction) &&
      GPIO_IS_VALID_GPIO(kLeftMotor.hall) && GPIO_IS_VALID_GPIO(kRightMotor.hall)) {
    const bool leftReady = leftWheel.begin();
    const bool rightReady = rightWheel.begin();
    motorsReady = leftReady && rightReady;
    if (!motorsReady) {
      leftWheel.disable();
      rightWheel.disable();
    }
  }

  WiFi.persistent(false);
  WiFi.mode(WIFI_AP);
  if (!WiFi.softAP(kDriveSsid, kDrivePassword, 1, 0, 1)) {
    Serial.println("Drive controller could not start.");
    return;
  }
  bootId = esp_random();
  startEduroam();
  if (!startServer()) {
    Serial.println("Drive controller could not start.");
    return;
  }

  Serial.printf("Join Wi-Fi %s and open http://%s/\n", kDriveSsid, WiFi.softAPIP().toString().c_str());
  Serial.println(motorsReady ? "Motor PWM ready." : "Motor pins unavailable. Drive disabled.");
  Serial.println(calibrated ? "Wheel RPM calibrated. Type help for tuning."
                            : "Wheel RPM needs calibration. Type help for tuning.");
}

void loop() {
  portENTER_CRITICAL(&controlLock);
  const uint32_t now = millis();
  expireCommand(now);
  const bool ready = readyToDrive();
  const bool hardwareReady = motorsReady;
  const float dutyLimit = bench.active() ? kBenchDutyLimit : kMaxDutyFraction;
  const bool bypassLeft = hallOverride && (fault & LeftHall);
  const bool bypassRight = hallOverride && (fault & RightHall);
  WheelSpeeds targets = ready ? command.targets : WheelSpeeds{};
  const TuneRequest pendingTune = tuning;
  portEXIT_CRITICAL(&controlLock);

  const int64_t nowUs = esp_timer_get_time();
  static int64_t lastUpdateUs = 0;
  const float seconds = lastUpdateUs
    ? std::clamp((nowUs - lastUpdateUs) / 1'000'000.f, 0.001f, 0.1f)
    : kUpdatePeriodMs / 1000.f;
  lastUpdateUs = nowUs;

  if (hardwareReady) {
    const WheelResult leftResult = leftWheel.update(targets.left, now, nowUs, seconds, bypassLeft, dutyLimit);
    const WheelResult rightResult = rightWheel.update(targets.right, now, nowUs, seconds, bypassRight, dutyLimit);
    if (leftResult != WheelResult::Ok || rightResult != WheelResult::Ok) {
      const auto leftStop = leftWheel.coast(now);
      const auto rightStop = rightWheel.coast(now);
      const bool pwmFailed = leftResult == WheelResult::PwmError || rightResult == WheelResult::PwmError ||
                             leftStop != WheelResult::Ok || rightStop != WheelResult::Ok;
      if (pwmFailed) {
        leftWheel.disable();
        rightWheel.disable();
      }
      portENTER_CRITICAL(&controlLock);
      motorsReady = !pwmFailed;
      if (pwmFailed) {
        fault = Pwm;
      } else {
        if (leftResult == WheelResult::FeedbackLost) fault |= LeftHall;
        if (rightResult == WheelResult::FeedbackLost) fault |= RightHall;
      }
      hallOverride = false;
      if (bench.active()) finishBench("fault");
      else command = {};
      portEXIT_CRITICAL(&controlLock);
      targets = {};
    }
  }

  serviceSerial(targets.left == 0 && targets.right == 0, now, targets);
  bool tuningOk = false;
  if (pendingTune.id && !leftWheel.duty() && !rightWheel.duty()) {
    if (pendingTune.side == 's') {
      tuningOk = saveTuning();
    } else {
      applyGains(pendingTune.side == 'l' ? leftWheel : rightWheel, pendingTune.gains);
      tuningOk = true;
    }
  }

  portENTER_CRITICAL(&controlLock);
  calibrated = leftWheel.tune.pulsesPerRev && rightWheel.tune.pulsesPerRev;
  measured = {leftWheel.rpm(), rightWheel.rpm()};
  sample = {now, targets,
            {measured.left, leftWheel.duty(), leftWheel.pidTerms(), leftWheel.tune},
            {measured.right, rightWheel.duty(), rightWheel.pidTerms(), rightWheel.tune}};
  if (pendingTune.id) {
    finishedTune = pendingTune.id;
    tuneSucceeded = tuningOk;
    tuning = {};
  }
  const char* completed = benchEnd;
  benchEnd = nullptr;
  portEXIT_CRITICAL(&controlLock);
  if (completed) Serial.printf("BENCH END %s\n", completed);

  serviceEduroam(now);
  delay(kUpdatePeriodMs);
}
