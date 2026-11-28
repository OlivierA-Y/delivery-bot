#pragma once

#include "../../drive-controller/src/speed_control.h"

namespace hotelbot {

struct WheelConfig {
  int pwm;
  int rotation;
  int hallA;
  int hallB;
  int hallC;
  unsigned transitionsPerRevolution;
  int hallForwardSign;
  int forwardLevel;
  WheelTune gains;
};

// Set the last three calibration fields after the hand-turn and low-duty checks.
// 0 transitions, 0 Hall sign, and -1 direction leave the board disarmed.
#if CONFIG_IDF_TARGET_ESP32
// The DevKit bench uses 90 transitions per turn. Raw forward counts decrease on both wheels.
inline constexpr WheelTune kDevKitWheelGains{0.00089f, 0.0055f};
inline constexpr WheelConfig kLeftWheel{15, 23, 19, 21, 22, 90, -1, 0, kDevKitWheelGains};
inline constexpr WheelConfig kRightWheel{16, 4, 17, 5, 18, 90, -1, 1, kDevKitWheelGains};
inline constexpr unsigned kPwmHz = 490;
#elif CONFIG_IDF_TARGET_ESP32S3
inline constexpr WheelConfig kLeftWheel{4, 6, 8, 10, 11, 0, 0, -1, {}};
inline constexpr WheelConfig kRightWheel{5, 7, 9, 12, 13, 0, 0, -1, {}};
inline constexpr unsigned kPwmHz = 2000;
#else
#error Select the ESP32 DevKit V1 or Waveshare ESP32-S3-Zero PlatformIO environment.
#endif
inline constexpr bool kEnableBenchMoves = false;
inline constexpr float kMaxWheelRpm = 200.f;
inline constexpr float kMaxWheelDegPerSecond = kMaxWheelRpm * 6.f;
inline constexpr float kMaxPwmDuty = .35f;
inline constexpr unsigned kSerialBaud = 115200;

constexpr bool calibrated(const WheelConfig& wheel) {
  return wheel.transitionsPerRevolution >= 6 && wheel.transitionsPerRevolution <= 10000 &&
         (wheel.hallForwardSign == 1 || wheel.hallForwardSign == -1) &&
         (wheel.forwardLevel == 0 || wheel.forwardLevel == 1);
}

}  // namespace hotelbot
