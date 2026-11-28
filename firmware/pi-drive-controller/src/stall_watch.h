#pragma once

#include <cstdint>

namespace hotelbot {

inline constexpr uint64_t kWheelStallMs = 5000;

class StallWatch {
 public:
  void reset() { watching_ = false; }

  bool update(uint64_t now, int64_t count, bool powered) {
    if (!powered) {
      reset();
      return false;
    }
    if (!watching_ || count != lastCount_) {
      watching_ = true;
      lastCount_ = count;
      lastMotionAt_ = now;
    }
    return now - lastMotionAt_ >= kWheelStallMs;
  }

 private:
  bool watching_ = false;
  int64_t lastCount_ = 0;
  uint64_t lastMotionAt_ = 0;
};

}  // namespace hotelbot
