#pragma once

#include <Arduino.h>
#include <esp_timer.h>

struct HallReading {
  uint32_t pulses = 0;
  int64_t lastEdgeUs = 0;
  int64_t periodUs = 0;
};

inline constexpr int64_t kMaxHallPeriodUs = 1'500'000;

class HallFeedback {
 public:
  void begin(uint8_t pin) {
    pinMode(pin, INPUT);
    attachInterruptArg(pin, onEdge, this, RISING);
  }

  HallReading read() {
    portENTER_CRITICAL(&lock_);
    const HallReading snapshot = reading_;
    portEXIT_CRITICAL(&lock_);
    return snapshot;
  }

 private:
  static void ARDUINO_ISR_ATTR onEdge(void* context) {
    auto* hall = static_cast<HallFeedback*>(context);
    const int64_t now = esp_timer_get_time();
    portENTER_CRITICAL_ISR(&hall->lock_);
    const int64_t period = now - hall->reading_.lastEdgeUs;
    if (hall->reading_.lastEdgeUs == 0 || period >= 150) {
      if (hall->reading_.lastEdgeUs != 0) hall->recordPeriod(period);
      hall->reading_.lastEdgeUs = now;
      ++hall->reading_.pulses;
    }
    portEXIT_CRITICAL_ISR(&hall->lock_);
  }

  void ARDUINO_ISR_ATTR recordPeriod(int64_t period) {
    const int64_t previous = reading_.periodUs;
    // One extra edge can split a real interval into two short ones. Require a
    // third short interval before accepting a sudden speed increase.
    if (previous && period * 4 < previous * 3) {
      if (++shortIntervals_ < 3) return;
    }
    shortIntervals_ = 0;
    // A pause between runs is not a usable speed measurement.
    reading_.periodUs = period > kMaxHallPeriodUs ? 0 : period;
  }

  portMUX_TYPE lock_ = portMUX_INITIALIZER_UNLOCKED;
  HallReading reading_;
  uint8_t shortIntervals_ = 0;
};

inline float estimateRpm(const HallReading& hall, unsigned pulsesPerRev, int64_t nowUs) {
  if (!pulsesPerRev || !hall.periodUs || hall.periodUs > kMaxHallPeriodUs) return 0;
  const int64_t age = nowUs - hall.lastEdgeUs;
  const int64_t timeout = constrain(3 * hall.periodUs, int64_t{250'000}, kMaxHallPeriodUs);
  if (age >= timeout) return 0;
  return 60'000'000.f / (pulsesPerRev * std::max(hall.periodUs, age));
}
