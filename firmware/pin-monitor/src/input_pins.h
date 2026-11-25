#pragma once

// Enter one to eight distinct GPIO numbers in display order for your target.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
constexpr int kInputPins[] = {4, 5};
#else
constexpr int kInputPins[] = {34, 35};
#endif
