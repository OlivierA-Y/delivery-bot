#include <Arduino.h>
#include <driver/gpio.h>
#include <esp_timer.h>
#include <freertos/queue.h>

#include "input_pins.h"

namespace {
constexpr uint32_t kBaud = 230400;
constexpr uint32_t kSamplePeriodUs = 200;
constexpr size_t kFrameSamples = 64;
constexpr size_t kPinCount = sizeof(kInputPins) / sizeof(kInputPins[0]);
static_assert(kPinCount >= 1 && kPinCount <= 8, "Configure one to eight GPIO inputs");

struct Frame {
  int64_t firstSampleUs = 0;
  size_t count = 0;
  uint8_t levels[kFrameSamples] = {};
};

QueueHandle_t frames = nullptr;
Frame sampling;
int64_t lastSampleUs = 0;
bool running = false;

bool validInputs() {
  uint64_t seen = 0;
  for (const int pin : kInputPins) {
    if (pin < 0 || !GPIO_IS_VALID_GPIO(pin)) return false;
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    // Keep native USB and the flash/PSRAM bus available.
    if (pin == 19 || pin == 20 || (pin >= 26 && pin <= 37)) return false;
#else
    // UART0 carries the stream. Other reserved pins can be wired to flash or PSRAM.
    if (pin == 1 || pin == 3 || (pin >= 6 && pin <= 11) || pin == 16 || pin == 17) return false;
#endif
    const uint64_t bit = 1ULL << pin;
    if (seen & bit) return false;
    seen |= bit;
  }
  return true;
}

void queueFrame() {
  // Keep the sampler independent of serial I/O. A full queue drops a frame.
  xQueueSend(frames, &sampling, 0);
  sampling.count = 0;
}

void sampleInputs(void*) {
  const int64_t now = esp_timer_get_time();
  const int64_t interval = now - lastSampleUs;
  if (lastSampleUs && interval < kSamplePeriodUs / 2) return;
  if (sampling.count && interval > kSamplePeriodUs * 2) queueFrame();
  if (sampling.count == 0) sampling.firstSampleUs = now;

  uint8_t levels = 0;
  for (size_t index = 0; index < kPinCount; ++index) {
    if (gpio_get_level(static_cast<gpio_num_t>(kInputPins[index]))) levels |= 1U << index;
  }
  sampling.levels[sampling.count++] = levels;
  lastSampleUs = now;
  if (sampling.count == kFrameSamples) queueFrame();
}

void sendConfiguration() {
  char line[64];
  int length = snprintf(line, sizeof(line), "P,%lu", static_cast<unsigned long>(kSamplePeriodUs));
  for (const int pin : kInputPins) {
    length += snprintf(line + length, sizeof(line) - length, ",%d", pin);
  }
  line[length++] = '\n';
  Serial.write(reinterpret_cast<const uint8_t*>(line), length);
}

void sendFrame(const Frame& frame) {
  constexpr char hex[] = "0123456789abcdef";
  char line[32 + kFrameSamples * 2];
  size_t length = snprintf(line, sizeof(line), "D,%lld,", static_cast<long long>(frame.firstSampleUs));
  for (size_t index = 0; index < frame.count; ++index) {
    const uint8_t levels = frame.levels[index];
    line[length++] = hex[levels >> 4];
    line[length++] = hex[levels & 0x0f];
  }
  line[length++] = '\n';
  Serial.write(reinterpret_cast<const uint8_t*>(line), length);
}

bool startSampler() {
  if (!validInputs()) return false;
  for (const int pin : kInputPins) pinMode(pin, INPUT);

  frames = xQueueCreate(8, sizeof(Frame));
  if (!frames) return false;

  const esp_timer_create_args_t config{
    .callback = sampleInputs,
    .dispatch_method = ESP_TIMER_TASK,
    .name = "pin_sampler",
    .skip_unhandled_events = true,
  };
  esp_timer_handle_t timer = nullptr;
  return esp_timer_create(&config, &timer) == ESP_OK &&
         esp_timer_start_periodic(timer, kSamplePeriodUs) == ESP_OK;
}
}  // namespace

void setup() {
  Serial.begin(kBaud);
  running = startSampler();
}

void loop() {
  static uint32_t lastConfigMs = 0;
  const uint32_t now = millis();
  if (lastConfigMs == 0 || now - lastConfigMs >= 500) {
    lastConfigMs = now;
    if (running) {
      sendConfiguration();
    } else {
      Serial.println("E,Sampler unavailable. Check the configured GPIO inputs.");
    }
  }

  Frame frame;
  if (running && xQueueReceive(frames, &frame, pdMS_TO_TICKS(20)) == pdTRUE) {
    sendFrame(frame);
  } else {
    delay(1);
  }
}
