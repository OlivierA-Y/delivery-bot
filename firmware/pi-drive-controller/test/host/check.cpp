#include "hall_decoder.h"
#include "protocol.h"
#include "stall_watch.h"

#include <cassert>
#include <cmath>
#include <cstring>
#include <string>

using namespace hotelbot;

namespace {
Frame frame(const char* json) {
  Frame result;
  assert(parseFrame(json, result));
  return result;
}

Reply receive(Protocol& controller, const char* json, uint64_t now, const Feedback& feedback = {}) {
  return controller.accept(frame(json), now, feedback);
}

constexpr char kHello[] = R"({"type":"hello","v":1,"session":"s1"})";
constexpr char kHello2[] = R"({"type":"hello","v":1,"session":"s2"})";
constexpr char kArm[] = R"({"type":"arm","session":"s1","seq":1})";
constexpr char kSpeed[] = R"({"type":"set_wheel_speeds","session":"s1","seq":2,"left_deg_s":90,"right_deg_s":-110})";
constexpr char kMove[] = R"({"type":"move_wheels","session":"s1","seq":2,"move_id":"m1","left_deg":-45,"right_deg":90,"max_deg_s":90})";
constexpr char kKeepAlive[] = R"({"type":"keepalive","session":"s1","seq":3,"move_id":"m1"})";
constexpr char kStop[] = R"({"type":"stop","session":"s1","seq":4})";

void checkFraming() {
  LineReader lines;
  for (const char* c = kHello; *c; ++c) assert(lines.append(*c) == LineReader::Result::Pending);
  assert(lines.append('\r') == LineReader::Result::Pending);
  assert(lines.append('\n') == LineReader::Result::Frame);
  assert(frame(lines.line()).version == 1);

  for (size_t i = 0; i <= kMaxFrame; ++i) lines.append('a');
  assert(lines.append('\n') == LineReader::Result::Invalid);
  lines.append(0);
  assert(lines.append('\n') == LineReader::Result::Invalid);
  for (const char* c = kArm; *c; ++c) lines.append(*c);
  assert(lines.append('\n') == LineReader::Result::Frame && frame(lines.line()).kind == Kind::Arm);

  Frame unused;
  const char* invalid[] = {
    R"({"type":"hello","session":"s1","v":01})",
    R"({"type":"hello","session":"s1","v":1.})",
    R"({"type":"hello","session":"s1","v":1} {})",
    R"({"type":"hello","session":"s1","session":"s2","v":1})",
    R"({"type":"hello","session":"a\u0000s","v":1})",
    R"({"type":"hello","session":"s1","v":1,"deep":{"a":1}})",
    R"({"type":"arm","session":"s1","seq":4294967296})",
    R"({"type":"set_wheel_speeds","session":"s1","seq":2,"left_deg_s":1e300,"right_deg_s":90})",
    R"({"type":"set_wheel_speeds","session":"s1","seq":2,"left_deg_s":false,"right_deg_s":75})",
  };
  for (const char* json : invalid) assert(!parseFrame(json, unused));
}

void checkSpeedAndWatchdog() {
  Protocol controller({120, 120, 240, false});
  assert(!controller.armed() && !controller.ready());
  assert(receive(controller, kSpeed, 0).error);
  assert(receive(controller, kHello, 1).type == Reply::Type::HelloAck && !controller.armed());
  const Feedback uncalibrated{{}, {}, Uncalibrated};
  assert(receive(controller, kArm, 5, uncalibrated).error && !controller.armed());
  assert(!receive(controller, kArm, 20).error && controller.armed());
  assert(!receive(controller, kSpeed, 50).error && controller.mode() == Mode::Speed);
  assert(controller.targets().left == 90 && controller.targets().right == -110);
  assert(receive(controller, kHello, 55).type == Reply::Type::HelloAck && controller.armed());
  assert(receive(controller, kSpeed, 100).error);
  assert(receive(controller, kKeepAlive, 150).error);
  assert(receive(controller, R"({"type":"set_wheel_speeds","session":"s1","seq":3,"left_deg_s":241,"right_deg_s":-110})",
                 250).error);
  controller.tick(299, {});
  assert(controller.armed());
  controller.tick(300, {});
  assert(!controller.armed() && controller.targets().left == 0 && (controller.faults() & LinkTimeout));
  assert(receive(controller, kArm, 310).error && receive(controller, kHello, 320).error);
  assert(receive(controller, kHello2, 330).type == Reply::Type::HelloAck && !controller.armed());
  assert(receive(controller, kSpeed, 350).error);
  assert(!receive(controller, R"({"type":"arm","session":"s2","seq":1})", 360).error);
  controller.disconnect();
  assert(!controller.armed() && receive(controller, kHello2, 361).error);
}

void checkStopAndFault() {
  Protocol controller({120, 120, 240, false});
  receive(controller, kHello, 10);
  receive(controller, kArm, 20);
  assert(receive(controller, kMove, 22).error && controller.mode() == Mode::Idle);
  receive(controller, kSpeed, 30);
  assert(!receive(controller, kStop, 40).error && !controller.armed());
  assert(!receive(controller, kStop, 41).error && controller.lastSequence() == 4);
  assert(!receive(controller, R"({"type":"arm","session":"s1","seq":5})", 50).error);
  controller.tick(65, {{}, {}, LeftStall});
  assert(!controller.armed() && (controller.faults() & LeftStall));
  assert(receive(controller, R"({"type":"arm","session":"s1","seq":6})", 66).error);
  assert(receive(controller, R"({"type":"hello","v":2,"session":"s2"})", 67).error);
  assert(receive(controller, kHello, 68).error);
}

void checkJoystickSpeedLimit() {
  Protocol controller({90, 90, 1200, true});
  receive(controller, kHello, 10);
  receive(controller, kArm, 20);
  const auto move = receive(controller,
      R"({"type":"move_wheels","session":"s1","seq":2,"move_id":"m1","left_deg":45,"right_deg":45,"max_deg_s":241})",
      25);
  assert(move.error && std::strcmp(move.error, "move_limit") == 0);
  const auto speed = receive(controller,
      R"({"type":"set_wheel_speeds","session":"s1","seq":2,"left_deg_s":1200,"right_deg_s":-1200})", 30);
  assert(!speed.error && controller.targets().left == 1200 && controller.targets().right == -1200);
  const auto over = receive(controller,
      R"({"type":"set_wheel_speeds","session":"s1","seq":3,"left_deg_s":1201,"right_deg_s":0})", 40);
  assert(over.error && std::strcmp(over.error, "speed_limit") == 0);
  controller.tick(280, {});
  assert(!controller.armed() && controller.targets().left == 0 && (controller.faults() & LinkTimeout));
}

void checkWheelStallTiming() {
  StallWatch watch;
  assert(!watch.update(10'000, 20, false));
  assert(!watch.update(11'000, 20, true));
  assert(!watch.update(15'999, 20, true));
  assert(watch.update(16'000, 20, true));

  assert(!watch.update(16'100, 20, false));
  assert(!watch.update(25'000, 20, true));
  assert(!watch.update(29'900, 19, true));
  assert(!watch.update(34'899, 19, true));
  assert(watch.update(34'900, 19, true));
  watch.reset();
  assert(!watch.update(40'000, 19, true));
}

void checkPosition() {
  Protocol controller({120, 120, 240, true});
  const Feedback baseline{{1000, 0}, {-200, 0}};
  receive(controller, kHello, 10, baseline);
  receive(controller, kArm, 20, baseline);
  assert(!receive(controller, kMove, 30, baseline).error);
  assert(controller.lastMove()->status == MoveStatus::Accepted);
  controller.tick(35, baseline);
  assert(controller.targets().left == -45 && controller.targets().right == 90);
  const Feedback partial{{994, -40}, {-188, 85}};
  const Reply retry = receive(controller, kMove, 45, partial);
  assert(retry.move && retry.move->status == MoveStatus::Moving);
  assert(receive(controller, kSpeed, 46, partial).error);
  assert(!receive(controller, kKeepAlive, 50, partial).error);
  const Feedback arrived{{985, 0}, {-170, 0}};
  controller.tick(60, arrived);
  assert(controller.targets().left == 0 && controller.targets().right == 0);
  controller.tick(160, arrived);
  assert(controller.lastMove()->status == MoveStatus::Done && !controller.armed());
  assert(receive(controller, kMove, 170, arrived).move->status == MoveStatus::Done);
  assert(controller.lastSequence() == 3);
  receive(controller, kStop, 180, arrived);
  receive(controller, R"({"type":"arm","session":"s1","seq":5})", 200, arrived);
  const char* nextMove = R"({"type":"move_wheels","session":"s1","seq":6,"move_id":"m2","left_deg":45,"right_deg":45,"max_deg_s":40})";
  assert(!receive(controller, nextMove, 220, arrived).error);
  assert(receive(controller, kMove, 240, arrived).move->status == MoveStatus::Done);
  assert(controller.lastMove()->status == MoveStatus::Moving);
  controller.tick(470, arrived);
  assert(!controller.armed() && controller.lastMove()->status == MoveStatus::Canceled);
}

void checkHall() {
  HallSample hall;
  constexpr uint8_t phase[] = {1, 5, 4, 6, 2, 3};
  hall.phase = hallPhase(phase[0]);
  for (int index = 1; index <= 6; ++index) decodeHall(hall, hallPhase(phase[index % 6]), index * 10'000, 1);
  assert(hall.count == 6 && hall.errors == 0);
  assert(std::abs(hallSpeed(hall, 120, 60'000) - 300.f) < .001f);
  for (int index = 6; index >= 1; --index) decodeHall(hall, hallPhase(phase[(index - 1) % 6]), 130'000 - index * 10'000, 1);
  assert(hall.count == 0 && hall.direction == -1 && hallSpeed(hall, 120, 122'000) < 0);
  assert(hallSpeed(hall, 120, 400'000) == 0);
  decodeHall(hall, hallPhase(6), 420'000, 1);
  assert(hall.count == 0 && hall.errors == 1);
  decodeHall(hall, hallPhase(0), 430'000, 1);
  decodeHall(hall, hallPhase(5), 440'000, -1);
  assert(hall.count == 0 && hall.errors == 2);
  decodeHall(hall, hallPhase(4), 450'000, -1);
  assert(hall.count == -1);
}

void checkHallTimingFilter() {
  HallSample hall;
  hall.phase = 0;
  decodeHall(hall, 1, 10'000, 1);
  decodeHall(hall, 2, 210'000, 1);
  assert(hall.periodUs == 200'000 && hallSpeed(hall, 90, 210'000) == 20);
  decodeHall(hall, 3, 310'000, 1);
  decodeHall(hall, 4, 410'000, 1);
  assert(hall.count == 4 && hallSpeed(hall, 90, 410'000) == 20);
  decodeHall(hall, 5, 610'000, 1);
  decodeHall(hall, 0, 710'000, 1);
  decodeHall(hall, 1, 810'000, 1);
  assert(hallSpeed(hall, 90, 810'000) == 20);
  decodeHall(hall, 2, 910'000, 1);
  assert(hall.count == 8 && hallSpeed(hall, 90, 910'000) == 40);
  decodeHall(hall, 1, 1'010'000, 1);
  assert(hall.count == 7 && hall.periodUs == 0);
  decodeHall(hall, 0, 1'110'000, 1);
  assert(hallSpeed(hall, 90, 1'110'000) == -40);
  decodeHall(hall, 5, 2'510'000, 1);
  assert(hall.periodUs == 0);
  decodeHall(hall, 4, 2'710'000, 1);
  assert(hallSpeed(hall, 90, 2'710'000) == -20);
}

void checkHallPhaseFilter() {
  assert(hallGlitchUs(90, 200) == 100);
  assert(hallGlitchUs(10'000, 200) == 7);
  HallFilter hall(hallGlitchUs(90, 200));
  hall.begin(0);
  hall.observe(-1, 1'000, -1);
  hall.observe(0, 1'050, -1);
  hall.observe(0, 1'150, -1);
  assert(hall.sample().errors == 0 && hall.sample().count == 0);

  hall.observe(1, 10'000, -1);
  hall.observe(2, 20'000, -1);
  assert(hall.sample().count == -1 && hall.sample().lastEdgeUs == 10'000);
  hall.observe(2, 20'100, -1);
  assert(hall.sample().count == -2 && hall.sample().periodUs == 10'000);

  hall.observe(-1, 21'000, -1);
  hall.observe(-1, 21'099, -1);
  assert(hall.sample().phase == 2 && hall.sample().errors == 0);
  hall.observe(-1, 21'100, -1);
  assert(hall.sample().phase == -1 && hall.sample().errors == 1);
}

void checkDirectionFault() {
  Protocol controller({90, 90, 1200, false});
  receive(controller, kHello, 10);
  receive(controller, kArm, 20);
  receive(controller, kSpeed, 25);
  const Feedback feedback{{}, {}, RightDirection};
  controller.tick(35, feedback);
  assert(!controller.armed() && controller.faults() == RightDirection);
  char output[640];
  formatState(output, sizeof(output), controller, feedback, "b7", 35);
  assert(std::strstr(output, "\"faults\":[\"right_direction\"]"));
}

void checkOutput() {
  Protocol controller({120, 120, 240, true});
  char output[512];
  const auto hello = receive(controller, kHello, 1);
  const int helloLength = formatReply(output, sizeof(output), hello, frame(kHello), "b7", true, 1200.f);
  assert(helloLength > 0 && std::strstr(output, "\"capabilities\":[\"set_wheel_speeds\",\"move_wheels\"]"));
  assert(std::strstr(output, "\"max_wheel_deg_s\":1200"));
  const Feedback feedback{{-16, -89}, {21, 109}};
  const int stateLength = formatState(output, sizeof(output), controller, feedback, "b7", 1280);
  assert(stateLength > 0 && stateLength < static_cast<int>(sizeof(output)));
  assert(std::strstr(output, "\"left_count\":-16") && std::strstr(output, "\"armed\":false") &&
         std::strstr(output, "\"sample_ms\":1280") && std::strstr(output, "\"last_seq\":null") &&
         output[stateLength - 1] == '\n');
}
}  // namespace

int main() {
  checkFraming();
  checkSpeedAndWatchdog();
  checkStopAndFault();
  checkJoystickSpeedLimit();
  checkWheelStallTiming();
  checkPosition();
  checkHall();
  checkHallTimingFilter();
  checkHallPhaseFilter();
  checkDirectionFault();
  checkOutput();
}
