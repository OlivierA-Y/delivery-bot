#pragma once

#include <Arduino.h>

struct MotorPins {
  int pwm;
  int direction;
  uint8_t forwardLevel;
  int hall;
  int brake;
};

// PWM GPIO, direction GPIO, forward level, Hall GPIO, brake-switch GPIO.
constexpr MotorPins kLeftMotor{15, 4, LOW, 22, 12};
constexpr MotorPins kRightMotor{16, 23, HIGH, 18, 13};
constexpr uint32_t kJoystickBrakeMs = 500;
constexpr int kMaxWheelRpm = 240;
constexpr float kMaxDutyFraction = 0.55f;
constexpr int kHallOverrideRpm = 40;
constexpr float kHallOverrideDuty = 0.30f;
constexpr char kDriveSsid[] = "HotelBot";
constexpr char kDrivePassword[] = "hotelbot-drive";
