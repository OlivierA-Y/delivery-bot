#include <Arduino.h>

namespace {
constexpr uint8_t kPwmPin = 15;
constexpr uint32_t kPwmFrequencyHz = 490;
constexpr uint8_t kPwmBits = 8;
constexpr uint32_t kMaxDuty = (1U << kPwmBits) - 1;
constexpr uint32_t kRampMs = 5000;
constexpr uint32_t kHoldMs = 500;
constexpr uint32_t kRampDownStartMs = kRampMs + kHoldMs;
constexpr uint32_t kRampDownEndMs = kRampDownStartMs + kRampMs;
constexpr uint32_t kCycleMs = kRampDownEndMs + kHoldMs;

bool pwmReady = false;
bool sweepActive = false;
uint32_t startedAt = 0;
uint32_t lastDuty = 0;

void disablePwm() {
  ledcDetach(kPwmPin);
  pinMode(kPwmPin, OUTPUT);
  digitalWrite(kPwmPin, LOW);
  pwmReady = false;
  sweepActive = false;
  Serial.println("PWM error. GPIO15 set low. Reset to retry.");
}

void stopSweep() {
  if (!ledcWrite(kPwmPin, 0)) {
    disablePwm();
    return;
  }
  sweepActive = false;
  Serial.println("PWM at 0%. Send s for another sweep.");
}

uint32_t dutyAt(uint32_t elapsed) {
  if (elapsed < kRampMs) {
    return elapsed * kMaxDuty / kRampMs;
  }
  if (elapsed < kRampDownStartMs) {
    return kMaxDuty;
  }
  if (elapsed < kRampDownEndMs) {
    return (kRampDownEndMs - elapsed) * kMaxDuty / kRampMs;
  }
  return 0;
}
}  // namespace

void setup() {
  pinMode(kPwmPin, OUTPUT);
  digitalWrite(kPwmPin, LOW);

  Serial.begin(115200);
  pwmReady = ledcAttach(kPwmPin, kPwmFrequencyHz, kPwmBits) && ledcWrite(kPwmPin, 0);
  if (!pwmReady) {
    disablePwm();
  }
}

void loop() {
  if (Serial.available()) {
    const int command = Serial.read();
    if (command == '?') {
      Serial.println(pwmReady ? "GPIO15 PWM ready. s: repeat sweep, x: stop"
                              : "PWM unavailable. Reset to retry.");
    } else if (command == 'x' && sweepActive) {
      stopSweep();
    } else if (command == 's' && pwmReady && !sweepActive) {
      sweepActive = true;
      lastDuty = 0;
      startedAt = millis();
      Serial.println("Repeating motor sweep on GPIO15. Send x to stop.");
    }
  }

  if (sweepActive) {
    const uint32_t elapsed = millis() - startedAt;
    const uint32_t cycleElapsed = elapsed % kCycleMs;
    startedAt += elapsed - cycleElapsed;
    const uint32_t duty = dutyAt(cycleElapsed);
    if (duty != lastDuty) {
      if (!ledcWrite(kPwmPin, duty)) {
        disablePwm();
      } else {
        lastDuty = duty;
      }
    }
  }
  delay(1);
}
