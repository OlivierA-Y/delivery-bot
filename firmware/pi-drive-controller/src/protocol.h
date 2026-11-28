#pragma once

#include <array>
#include <cstddef>
#include <cstdint>

namespace hotelbot {

inline constexpr size_t kMaxFrame = 240;
inline constexpr size_t kIdBytes = 49;
inline constexpr uint64_t kWatchdogMs = 250;
inline constexpr uint64_t kStatePeriodMs = 40;
inline constexpr float kStoppedDegPerSecond = 8.f;

using Id = std::array<char, kIdBytes>;

enum Fault : uint16_t {
  Uncalibrated = 1 << 0,
  LeftHall = 1 << 1,
  RightHall = 1 << 2,
  LeftStall = 1 << 3,
  RightStall = 1 << 4,
  Pwm = 1 << 5,
  LinkTimeout = 1 << 6,
  MoveTimeout = 1 << 7,
  LeftDirection = 1 << 8,
  RightDirection = 1 << 9,
};

enum class Kind { Hello, Arm, Stop, Speed, Move, KeepAlive };
enum class Mode { Idle, Speed, Position };
enum class MoveStatus { Accepted, Moving, Done, Canceled, Fault };

struct Frame {
  Kind kind = Kind::Hello;
  Id session{};
  Id moveId{};
  uint32_t seq = 0;
  uint32_t version = 0;
  float left = 0;
  float right = 0;
  float maxSpeed = 0;
};

struct WheelState {
  int64_t count = 0;
  float speed = 0;
};

struct Feedback {
  WheelState left{};
  WheelState right{};
  uint16_t faults = 0;
};

struct Targets {
  float left = 0;
  float right = 0;
};

struct Settings {
  unsigned leftTransitions;
  unsigned rightTransitions;
  float maxSpeed;
  bool benchMoves;
};

struct MoveReport {
  Id id{};
  MoveStatus status = MoveStatus::Accepted;
};

struct Reply {
  enum class Type { State, HelloAck, Error } type = Type::State;
  const char* error = nullptr;
  const MoveReport* move = nullptr;
};

class Protocol {
 public:
  explicit Protocol(Settings settings) : settings_(settings) {}

  Reply accept(const Frame& frame, uint64_t now, const Feedback& feedback);
  void tick(uint64_t now, const Feedback& feedback);
  void disconnect();
  Targets targets() const { return targets_; }
  bool armed() const { return armed_; }
  bool hasSession() const { return session_[0] != '\0'; }
  const Id& session() const { return session_; }
  uint32_t lastSequence() const { return lastSeq_; }
  uint16_t faults() const { return currentFaults_ | latchedFaults_; }
  bool ready() const { return hasSession() && !needsHello_ && !faults(); }
  Mode mode() const { return mode_; }
  const MoveReport* lastMove() const { return moveCount_ ? &moves_[moveCount_ - 1] : nullptr; }

 private:
  void halt(MoveStatus outcome);
  void advanceMove(uint64_t now, const Feedback& feedback);
  void acceptSequence(uint32_t sequence, uint64_t now);

  Settings settings_;
  Id session_{};
  uint32_t lastSeq_ = 0;
  uint64_t lastCommandAt_ = 0;
  uint16_t currentFaults_ = 0;
  uint16_t latchedFaults_ = 0;
  bool armed_ = false;
  bool needsHello_ = true;
  Mode mode_ = Mode::Idle;
  Targets targets_{};

  // Retain every accepted ID until the next session. When full, fail closed.
  std::array<MoveReport, 24> moves_{};
  size_t moveCount_ = 0;
  int64_t leftBaseline_ = 0;
  int64_t rightBaseline_ = 0;
  int64_t leftGoal_ = 0;
  int64_t rightGoal_ = 0;
  float moveSpeed_ = 0;
  uint64_t moveStartedAt_ = 0;
  uint64_t settledAt_ = 0;
};

bool parseFrame(const char* line, Frame& frame);
int formatState(char* output, size_t capacity, const Protocol& protocol, const Feedback& feedback,
                const char* bootId, uint64_t now, const MoveReport* move = nullptr);
int formatReply(char* output, size_t capacity, const Reply& reply, const Frame& frame,
                const char* bootId, bool benchMoves, float maxWheelDegPerSecond);

class LineReader {
 public:
  enum class Result { Pending, Frame, Invalid };

  Result append(uint8_t byte);
  const char* line() const { return line_.data(); }
  void reset() { length_ = 0; overflow_ = false; }

 private:
  std::array<char, kMaxFrame + 1> line_{};
  size_t length_ = 0;
  bool overflow_ = false;
};

}  // namespace hotelbot
