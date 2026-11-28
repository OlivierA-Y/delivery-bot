#pragma once

#include <cstdint>

namespace hotelbot {

struct HallSample {
  int64_t count = 0;
  int64_t lastEdgeUs = 0;
  int64_t periodUs = 0;
  uint32_t errors = 0;
  int8_t phase = -1;
  int8_t direction = 0;
  uint8_t shortIntervals = 0;
};

inline constexpr int64_t kHallReacquireUs = 1'200'000;

constexpr int64_t hallGlitchUs(unsigned transitions, unsigned maxRpm) {
  if (!transitions || !maxRpm) return 100;
  const int64_t quarterPeriod = 15'000'000 / (static_cast<int64_t>(transitions) * maxRpm);
  return quarterPeriod > 100 ? 100 : (quarterPeriod < 1 ? 1 : quarterPeriod);
}

// Map the six legal Hall codes into a cyclic phase index. The frame is ABC.
constexpr int8_t hallPhase(uint8_t code) {
  switch (code) {
    case 1: return 0;
    case 5: return 1;
    case 4: return 2;
    case 6: return 3;
    case 2: return 4;
    case 3: return 5;
    default: return -1;
  }
}

// Pass the results of the atomic GPIO input capture to this decoder.
inline void decodeHall(HallSample& hall, int8_t next, int64_t nowUs, int sign) {
  const int8_t previous = hall.phase;
  if (next == previous) return;
  hall.phase = next;
  const int delta = next - previous;
  if (next < 0 || (previous >= 0 && delta != 1 && delta != -5 && delta != -1 && delta != 5)) {
    ++hall.errors;
    hall.direction = 0;
    hall.periodUs = 0;
    hall.lastEdgeUs = 0;
    hall.shortIntervals = 0;
    return;
  }
  if (previous < 0) return;
  const int direction = (delta == 1 || delta == -5 ? 1 : -1) * sign;
  const int64_t period = nowUs - hall.lastEdgeUs;
  if (!hall.lastEdgeUs || hall.direction != direction || period > kHallReacquireUs) {
    hall.periodUs = 0;
    hall.shortIntervals = 0;
  } else if (period > 0) {
    // A spurious edge can split one interval into two short ones. Keep the
    // previous speed until a third short interval confirms the new timing.
    if (!hall.periodUs || 4 * period >= 3 * hall.periodUs || ++hall.shortIntervals == 3) {
      hall.periodUs = period;
      hall.shortIntervals = 0;
    }
  }
  hall.lastEdgeUs = nowUs;
  hall.direction = static_cast<int8_t>(direction);
  hall.count += direction;
}

class HallFilter {
 public:
  explicit HallFilter(int64_t stableUs) : stableUs_(stableUs) {}

  void begin(int8_t phase) { candidate_ = sample_.phase = phase; }

  void observe(int8_t rawPhase, int64_t nowUs, int sign) {
    // An edge or the control loop can confirm a pending phase. Keep the
    // original timestamp so the filter delay does not affect wheel speed.
    if (candidate_ != sample_.phase && nowUs - changedUs_ >= stableUs_) {
      decodeHall(sample_, candidate_, changedUs_, sign);
    }
    if (rawPhase != candidate_) {
      candidate_ = rawPhase;
      changedUs_ = nowUs;
    }
  }

  HallSample sample() const { return sample_; }

 private:
  HallSample sample_{};
  const int64_t stableUs_;
  int64_t changedUs_ = 0;
  int8_t candidate_ = -1;
};

inline float hallSpeed(const HallSample& hall, unsigned transitions, int64_t nowUs) {
  if (!transitions || hall.phase < 0 || !hall.periodUs) return 0;
  const int64_t age = nowUs > hall.lastEdgeUs ? nowUs - hall.lastEdgeUs : 0;
  const int64_t period = hall.periodUs;
  const int64_t timeout = period < 83'333 ? 250'000 : (period > 400'000 ? kHallReacquireUs : period * 3);
  if (age >= timeout) return 0;
  return hall.direction * 360'000'000.f / (transitions * static_cast<float>(age > period ? age : period));
}

}  // namespace hotelbot
