#pragma once

#include <Arduino.h>

struct MotorPins {
  int pwm;
  int direction;
  uint8_t forwardLevel;
  int hall;
};

// PWM GPIO, direction GPIO, level that drives forward, Hall input GPIO.
constexpr MotorPins kLeftMotor{15, 4, LOW, 22};
constexpr MotorPins kRightMotor{16, 23, HIGH, 18};
constexpr int kMaxWheelRpm = 240;
constexpr float kMaxDutyFraction = 0.55f;
constexpr int kHallOverrideRpm = 40;
constexpr float kHallOverrideDuty = 0.30f;
constexpr char kDriveSsid[] = "HotelBot";
constexpr char kDrivePassword[] = "hotelbot-drive";
