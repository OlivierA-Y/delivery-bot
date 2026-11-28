#pragma once

#include <algorithm>
#include <cmath>

struct WheelTune {
  float kp = 0.005f;
  float ki = 0.010f;
  float kd = 0;
  unsigned pulsesPerRev = 0;
};

struct WheelSpeeds {
  float left = 0;
  float right = 0;
};

inline WheelSpeeds mixSpeeds(int turn, int forward, int limitRpm) {
  // Keep the established pivot direction near horizontal. Blend into the
  // opposite steering sense as the joystick moves toward forward or reverse.
  const float arc = std::min(std::abs(forward) / std::max(1.f, .5f * std::abs(turn)), 1.f);
  const float steering = turn * (2.f * arc - 1.f);
  const float left = (forward + steering) / 100.f;
  const float right = (forward - steering) / 100.f;
  const float scale = limitRpm / std::max({1.f, std::abs(left), std::abs(right)});
  return {left * scale, right * scale};
}

struct PidTerms {
  float p = 0;
  float i = 0;
  float d = 0;
};

class SpeedPid {
 public:
  PidTerms terms() const { return terms_; }

  void reset(float measured) {
    terms_ = {};
    lastMeasured_ = measured;
  }

  float update(const WheelTune& tune, float target, float measured, float seconds, float limit) {
    const float error = target - measured;
    const float rate = (measured - lastMeasured_) / seconds;
    lastMeasured_ = measured;
    terms_.p = tune.kp * error;
    terms_.d = -tune.kd * rate;
    const float proportionalAndDerivative = terms_.p + terms_.d;
    const float candidate = std::clamp(terms_.i + tune.ki * error * seconds, 0.f, limit);
    const float prospective = proportionalAndDerivative + candidate;
    if ((prospective < limit || error < 0) && (prospective > 0 || error > 0)) {
      terms_.i = candidate;
    }
    return std::clamp(proportionalAndDerivative + terms_.i, 0.f, limit);
  }

 private:
  PidTerms terms_;
  float lastMeasured_ = 0;
};
