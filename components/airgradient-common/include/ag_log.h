/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOG_H
#define AG_LOG_H

/**
 * Platform-portable logging macros.
 *
 * On ESP-IDF builds these are thin wrappers over ESP_LOG*.
 * Under TEST_HOST (native host test builds) all macros expand to silent
 * no-ops so translation units compile without ESP-IDF headers.
 *
 * Usage (identical to ESP_LOG*):
 *   AG_LOGI(TAG, "value = %d", value);
 */

#ifdef TEST_HOST

#define AG_LOGV(tag, fmt, ...)                                                                     \
  do {                                                                                             \
  } while (0)
#define AG_LOGD(tag, fmt, ...)                                                                     \
  do {                                                                                             \
  } while (0)
#define AG_LOGI(tag, fmt, ...)                                                                     \
  do {                                                                                             \
  } while (0)
#define AG_LOGW(tag, fmt, ...)                                                                     \
  do {                                                                                             \
  } while (0)
#define AG_LOGE(tag, fmt, ...)                                                                     \
  do {                                                                                             \
  } while (0)

#else

#include "esp_log.h"

#define AG_LOGV(tag, fmt, ...) ESP_LOGV(tag, fmt, ##__VA_ARGS__)
#define AG_LOGD(tag, fmt, ...) ESP_LOGD(tag, fmt, ##__VA_ARGS__)
#define AG_LOGI(tag, fmt, ...) ESP_LOGI(tag, fmt, ##__VA_ARGS__)
#define AG_LOGW(tag, fmt, ...) ESP_LOGW(tag, fmt, ##__VA_ARGS__)
#define AG_LOGE(tag, fmt, ...) ESP_LOGE(tag, fmt, ##__VA_ARGS__)

#endif // TEST_HOST

#endif // AG_LOG_H
