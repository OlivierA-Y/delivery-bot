#include <Arduino.h>
#include <esp_random.h>
#include <esp_timer.h>
#include <soc/gpio_reg.h>
#include <soc/soc.h>

#include <cstdio>

#include "drive_config.h"
#include "hall_decoder.h"
#include "protocol.h"
#include "stall_watch.h"

namespace hotelbot {
namespace {
constexpr uint64_t kControlPeriodMs = 10;
constexpr uint64_t kReverseOffMs = 100;
constexpr int kPwmBits = 8;
constexpr int kPwmMax = (1 << kPwmBits) - 1;
constexpr int kPwmRampStep = 3;

constexpr bool distinctPins() {
  constexpr int pins[] = {kLeftWheel.pwm, kLeftWheel.rotation, kLeftWheel.hallA, kLeftWheel.hallB, kLeftWheel.hallC,
                          kRightWheel.pwm, kRightWheel.rotation, kRightWheel.hallA, kRightWheel.hallB, kRightWheel.hallC};
  for (size_t i = 0; i < std::size(pins); ++i) {
    if (pins[i] < 0 || pins[i] >= 32) return false;
#if CONFIG_IDF_TARGET_ESP32S3
    if (pins[i] == 19 || pins[i] == 20) return false;
#else
    if (pins[i] == 1 || pins[i] == 3) return false;
#endif
    for (size_t j = i + 1; j < std::size(pins); ++j) {
      if (pins[i] == pins[j]) return false;
    }
  }
  return true;
}
static_assert(distinctPins(), "Wheel GPIOs must be distinct low-bank pins and leave the serial link pins free.");

class HallInput {
 public:
  explicit HallInput(const WheelConfig& config)
      : a_(config.hallA), b_(config.hallB), c_(config.hallC),
        sign_(config.hallForwardSign < 0 ? -1 : 1),
        filter_(hallGlitchUs(config.transitionsPerRevolution, kMaxWheelRpm)) {}

  void begin() {
    for (int pin : {a_, b_, c_}) pinMode(pin, INPUT);
    filter_.begin(currentPhase());
    for (int pin : {a_, b_, c_}) attachInterruptArg(pin, onEdge, this, CHANGE);
  }

  HallSample sample() {
    portENTER_CRITICAL(&lock_);
    filter_.observe(currentPhase(), esp_timer_get_time(), sign_);
    const HallSample current = filter_.sample();
    portEXIT_CRITICAL(&lock_);
    return current;
  }

 private:
  int8_t ARDUINO_ISR_ATTR currentPhase() const {
    const uint32_t input = REG_READ(GPIO_IN_REG);
    const uint8_t code = (((input >> a_) & 1U) << 2) | (((input >> b_) & 1U) << 1) | ((input >> c_) & 1U);
    return hallPhase(code);
  }

  static void ARDUINO_ISR_ATTR onEdge(void* sensor) {
    auto& hall = *static_cast<HallInput*>(sensor);
    portENTER_CRITICAL_ISR(&hall.lock_);
    hall.filter_.observe(hall.currentPhase(), esp_timer_get_time(), hall.sign_);
    portEXIT_CRITICAL_ISR(&hall.lock_);
  }

  int a_, b_, c_, sign_;
  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  HallFilter filter_;
};

class Wheel {
 public:
  Wheel(const WheelConfig& config, Fault hallFault, Fault stallFault, Fault directionFault)
      : config_(config), hall_(config), hallFault_(hallFault), stallFault_(stallFault),
        directionFault_(directionFault) {}

  void begin() {
    pinMode(config_.pwm, OUTPUT);
    digitalWrite(config_.pwm, LOW);
    pinMode(config_.rotation, OUTPUT);
    digitalWrite(config_.rotation, config_.forwardLevel == HIGH ? HIGH : LOW);
    hall_.begin();
    attached_ = ledcAttach(config_.pwm, kPwmHz, kPwmBits);
    pwmOk_ = attached_ && ledcWrite(config_.pwm, 0);
    if (!pwmOk_) disablePwm();
  }

  WheelState sample(int64_t nowUs) {
    const HallSample hall = hall_.sample();
    hallFaulted_ = hall.phase < 0 || hall.errors != lastHallErrors_;
    lastHallErrors_ = hall.errors;
    return {hall.count, hallSpeed(hall, config_.transitionsPerRevolution, nowUs)};
  }

  uint16_t faults() const {
    return (calibrated(config_) ? 0 : Uncalibrated) | (hallFaulted_ ? hallFault_ : 0) |
           (stalled_ ? stallFault_ : 0) | (pwmOk_ ? 0 : Pwm);
  }

  void stop(uint64_t now, float measuredSpeed) {
    setDuty(0, now);
    stallWatch_.reset();
    stalled_ = false;
    pid_.reset(measuredSpeed / 6.f);
  }

  uint16_t drive(float target, const WheelState& current, uint64_t now, float seconds) {
    if (!target || !pwmOk_) {
      stop(now, current.speed);
      return 0;
    }
    const bool forward = target > 0;
    const float rpm = (forward ? current.speed : -current.speed) / 6.f;
    if (forward != forward_) {
      stop(now, rpm * 6.f);
      if (pwmOk_ && std::abs(current.speed) <= kStoppedDegPerSecond &&
          (!hasPowered_ || now - zeroSince_ >= kReverseOffMs)) {
        digitalWrite(config_.rotation, forward ? config_.forwardLevel : !config_.forwardLevel);
        forward_ = forward;
      }
      return 0;
    }
    if (rpm < -kStoppedDegPerSecond / 6.f) {
      const bool wasPowered = duty_ != 0;
      stop(now, current.speed);
      return wasPowered ? directionFault_ : 0;
    }
    const float duty = pid_.update(config_.gains, std::abs(target) / 6.f, rpm, seconds, kMaxPwmDuty);
    const int desired = std::lround(duty * kPwmMax);
    setDuty(std::clamp(desired, duty_ - kPwmRampStep, duty_ + kPwmRampStep), now);
    stalled_ = stallWatch_.update(now, current.count, duty_ != 0);
    return 0;
  }

 private:
  void disablePwm() {
    if (attached_) ledcDetach(config_.pwm);
    attached_ = false;
    pwmOk_ = false;
    duty_ = 0;
    pinMode(config_.pwm, OUTPUT);
    digitalWrite(config_.pwm, LOW);
  }

  void setDuty(int value, uint64_t now) {
    if (!pwmOk_ || value == duty_) return;
    if (!ledcWrite(config_.pwm, value)) {
      disablePwm();
      return;
    }
    duty_ = value;
    if (value) hasPowered_ = true;
    else zeroSince_ = now;
  }

  const WheelConfig& config_;
  HallInput hall_;
  Fault hallFault_, stallFault_, directionFault_;
  SpeedPid pid_;
  StallWatch stallWatch_;
  bool attached_ = false;
  bool pwmOk_ = false;
  bool hallFaulted_ = false;
  bool stalled_ = false;
  bool forward_ = true;
  bool hasPowered_ = false;
  int duty_ = 0;
  uint32_t lastHallErrors_ = 0;
  uint64_t zeroSince_ = 0;
};

Wheel left{kLeftWheel, LeftHall, LeftStall, LeftDirection};
Wheel right{kRightWheel, RightHall, RightStall, RightDirection};
Protocol protocol{{kLeftWheel.transitionsPerRevolution, kRightWheel.transitionsPerRevolution,
                   kMaxWheelDegPerSecond, kEnableBenchMoves}};
LineReader reader;
char bootId[17]{};
char outgoing[640];
bool usbWasConnected = false;
uint64_t lastControlAt = 0;
uint64_t lastStateAt = 0;

void send(int length) {
  if (length > 0 && static_cast<size_t>(length) < sizeof(outgoing) && Serial.availableForWrite() >= length) {
    Serial.write(reinterpret_cast<const uint8_t*>(outgoing), length);
  }
}

void receive(uint64_t now, const Feedback& feedback) {
  // Bound work even if the host sends a continuous stream of invalid input.
  for (int i = 0; i < 256 && Serial.available(); ++i) {
    const int value = Serial.read();
    if (value < 0) break;
    if (reader.append(static_cast<uint8_t>(value)) != LineReader::Result::Frame) continue;
    Frame frame;
    if (!parseFrame(reader.line(), frame)) continue;

    const bool wasArmed = protocol.armed();
    const Reply reply = protocol.accept(frame, now, feedback);
    const bool halted = wasArmed && !protocol.armed();
    if (halted) {
      left.stop(now, feedback.left.speed);
      right.stop(now, feedback.right.speed);
    }
    if (reply.type != Reply::Type::State) {
      send(formatReply(outgoing, sizeof(outgoing), reply, frame, bootId,
                       kEnableBenchMoves, kMaxWheelDegPerSecond));
    } else if (reply.move || frame.kind == Kind::Move) {
      send(formatState(outgoing, sizeof(outgoing), protocol, feedback, bootId, now, reply.move));
    }
    if (halted) break;
  }
}
}  // namespace

void setupController() {
  left.begin();
  right.begin();
  std::snprintf(bootId, sizeof(bootId), "%08lx%08lx",
                static_cast<unsigned long>(esp_random()), static_cast<unsigned long>(esp_random()));
  Serial.setTxBufferSize(1024);
#if ARDUINO_USB_CDC_ON_BOOT && ARDUINO_USB_MODE
  Serial.setTxTimeoutMs(0);
#endif
  Serial.begin(kSerialBaud);
}

void updateController() {
  const int64_t nowUs = esp_timer_get_time();
  const uint64_t now = nowUs / 1000;
  if (now - lastControlAt < kControlPeriodMs) return;
  const float seconds = std::clamp((now - lastControlAt) / 1000.f, .001f, .05f);
  lastControlAt = now;

  Feedback feedback{left.sample(nowUs), right.sample(nowUs), 0};
  feedback.faults = left.faults() | right.faults();
  const bool connected = static_cast<bool>(Serial);
  if (connected) {
    receive(now, feedback);
  } else if (usbWasConnected) {
    protocol.disconnect();
    reader.reset();
  }
  usbWasConnected = connected;

  protocol.tick(now, feedback);
  const Targets target = protocol.targets();
  feedback.faults |= left.drive(target.left, feedback.left, now, seconds);
  feedback.faults |= right.drive(target.right, feedback.right, now, seconds);

  feedback.faults |= left.faults() | right.faults();
  if (feedback.faults) {
    protocol.tick(now, feedback);
    left.stop(now, feedback.left.speed);
    right.stop(now, feedback.right.speed);
  }

  if (connected && protocol.hasSession() && now - lastStateAt >= kStatePeriodMs) {
    lastStateAt = now;
    send(formatState(outgoing, sizeof(outgoing), protocol, feedback, bootId, now));
  }
}
}  // namespace hotelbot

void setup() {
  hotelbot::setupController();
}

void loop() {
  hotelbot::updateController();
  delay(1);
}
