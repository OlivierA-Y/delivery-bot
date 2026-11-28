#include "protocol.h"

#include <algorithm>
#include <cmath>

namespace hotelbot {
namespace {
constexpr float kMaxMoveDeg = 720.f;
constexpr float kMaxMoveDegPerSecond = 240.f;
constexpr float kMoveCorrection = 3.f;
constexpr uint64_t kMoveSettleMs = 100;
constexpr uint64_t kMaxMoveMs = 20'000;

Reply error(const char* code) {
  return {Reply::Type::Error, code};
}

bool validSpeed(float input, float maxSpeed) {
  return std::isfinite(input) && std::abs(input) <= maxSpeed;
}

bool stopped(const Feedback& feedback) {
  return std::abs(feedback.left.speed) <= kStoppedDegPerSecond &&
         std::abs(feedback.right.speed) <= kStoppedDegPerSecond;
}

float degrees(int64_t count, unsigned transitions) {
  return static_cast<float>(count) * 360.f / transitions;
}

float positionSpeed(int64_t remaining, int64_t goal, unsigned transitions, float commonDistance, float cap) {
  if (!goal || std::abs(remaining) <= 1) return 0;
  const float ratio = std::abs(degrees(goal, transitions)) / commonDistance;
  return std::clamp(kMoveCorrection * degrees(remaining, transitions), -cap * ratio, cap * ratio);
}
}  // namespace

void Protocol::halt(MoveStatus outcome) {
  if (mode_ == Mode::Position) moves_[moveCount_ - 1].status = outcome;
  armed_ = false;
  mode_ = Mode::Idle;
  targets_ = {};
}

void Protocol::disconnect() {
  halt(MoveStatus::Canceled);
  needsHello_ = true;
}

void Protocol::acceptSequence(uint32_t sequence, uint64_t now) {
  lastSeq_ = sequence;
  lastCommandAt_ = now;
}

void Protocol::tick(uint64_t now, const Feedback& feedback) {
  currentFaults_ = feedback.faults;
  if (!armed_) return;
  if (currentFaults_) {
    latchedFaults_ |= currentFaults_;
    halt(MoveStatus::Fault);
  } else if (now - lastCommandAt_ >= kWatchdogMs) {
    latchedFaults_ |= LinkTimeout;
    disconnect();
  } else if (mode_ == Mode::Position) {
    advanceMove(now, feedback);
  }
}

void Protocol::advanceMove(uint64_t now, const Feedback& feedback) {
  if (now - moveStartedAt_ >= kMaxMoveMs) {
    latchedFaults_ |= MoveTimeout;
    halt(MoveStatus::Fault);
    return;
  }
  const int64_t leftRemaining = leftGoal_ - (feedback.left.count - leftBaseline_);
  const int64_t rightRemaining = rightGoal_ - (feedback.right.count - rightBaseline_);
  targets_ = {};
  if (std::abs(leftRemaining) <= 1 && std::abs(rightRemaining) <= 1 && stopped(feedback)) {
    if (!settledAt_) settledAt_ = now;
    if (now - settledAt_ >= kMoveSettleMs) halt(MoveStatus::Done);
    return;
  }

  settledAt_ = 0;
  const float distance = std::max(std::abs(degrees(leftGoal_, settings_.leftTransitions)),
                                  std::abs(degrees(rightGoal_, settings_.rightTransitions)));
  targets_ = {positionSpeed(leftRemaining, leftGoal_, settings_.leftTransitions, distance, moveSpeed_),
              positionSpeed(rightRemaining, rightGoal_, settings_.rightTransitions, distance, moveSpeed_)};
  moves_[moveCount_ - 1].status = MoveStatus::Moving;
}

Reply Protocol::accept(const Frame& frame, uint64_t now, const Feedback& feedback) {
  tick(now, feedback);
  if (frame.kind == Kind::Hello) {
    if (frame.version != 1) {
      disconnect();
      return error("version");
    }
    if (session_ == frame.session) {
      if (needsHello_) return error("fresh_session_required");
      return {Reply::Type::HelloAck};
    }
    halt(MoveStatus::Canceled);
    session_ = frame.session;
    needsHello_ = false;
    lastSeq_ = 0;
    latchedFaults_ = 0;
    moveCount_ = 0;
    return {Reply::Type::HelloAck};
  }

  if (needsHello_ || !hasSession() || frame.session != session_) return error("session");

  if (frame.kind == Kind::Move) {
    for (size_t i = 0; i < moveCount_; ++i) {
      if (moves_[i].id == frame.moveId) return {Reply::Type::State, nullptr, &moves_[i]};
    }
  }
  if (frame.kind == Kind::Stop && frame.seq == lastSeq_ && !armed_) return {};
  if (frame.seq <= lastSeq_) return error("stale_sequence");

  switch (frame.kind) {
    case Kind::Stop:
      halt(MoveStatus::Canceled);
      break;
    case Kind::Arm:
      if (armed_ || !ready() || !stopped(feedback)) return error("cannot_arm");
      armed_ = true;
      break;
    case Kind::Speed:
      if (!armed_ || mode_ == Mode::Position) return error("cannot_drive");
      if (!validSpeed(frame.left, settings_.maxSpeed) || !validSpeed(frame.right, settings_.maxSpeed)) {
        return error("speed_limit");
      }
      targets_ = {frame.left, frame.right};
      mode_ = Mode::Speed;
      break;
    case Kind::Move: {
      if (!settings_.benchMoves) return error("position_disabled");
      if (!armed_ || mode_ != Mode::Idle || !stopped(feedback) ||
          !settings_.leftTransitions || !settings_.rightTransitions) return error("cannot_move");
      if (!validSpeed(frame.left, kMaxMoveDeg) || !validSpeed(frame.right, kMaxMoveDeg) ||
          !validSpeed(frame.maxSpeed, std::min(settings_.maxSpeed, kMaxMoveDegPerSecond)) ||
          frame.maxSpeed <= 0) return error("move_limit");
      const int64_t left = std::llround(frame.left * settings_.leftTransitions / 360.f);
      const int64_t right = std::llround(frame.right * settings_.rightTransitions / 360.f);
      if (std::abs(left) <= 1 && std::abs(right) <= 1) return error("move_too_small");
      if (moveCount_ == moves_.size()) return error("move_history_full");
      moves_[moveCount_++] = {frame.moveId, MoveStatus::Accepted};
      leftBaseline_ = feedback.left.count;
      rightBaseline_ = feedback.right.count;
      leftGoal_ = left;
      rightGoal_ = right;
      moveSpeed_ = frame.maxSpeed;
      moveStartedAt_ = now;
      settledAt_ = 0;
      mode_ = Mode::Position;
      break;
    }
    case Kind::KeepAlive:
      if (!armed_ || mode_ != Mode::Position || lastMove()->id != frame.moveId) return error("move_id");
      break;
    case Kind::Hello:
      break;
  }
  acceptSequence(frame.seq, now);
  return {};
}

LineReader::Result LineReader::append(uint8_t byte) {
  if (byte == '\n') {
    const bool invalid = overflow_ || !length_;
    if (length_ && line_[length_ - 1] == '\r') --length_;
    line_[length_] = '\0';
    reset();
    return invalid ? Result::Invalid : Result::Frame;
  }
  if (overflow_) return Result::Pending;
  if (length_ >= kMaxFrame || (byte < 32 && byte != '\r' && byte != '\t') || byte >= 127) {
    overflow_ = true;
  } else {
    line_[length_++] = static_cast<char>(byte);
  }
  return Result::Pending;
}

}  // namespace hotelbot
