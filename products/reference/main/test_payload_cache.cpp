#include "test_payload_cache.h"

#include "esp_log.h"

#include "backends/rtc_payload_cache_storage.h"
#include "measures_types.h"
#include "services/payload_cache.h"

static constexpr const char *TAG = "test_payload_cache";

static constexpr uint16_t CACHE_MAX  = 8;
static constexpr int      N_PAYLOADS = 3;

void run_test_payload_cache() {
  ESP_LOGI(TAG, "--- PayloadCache test start ---");

  RtcPayloadCacheStorage storage;
  PayloadCache           cache(storage, CACHE_MAX);

  // Restore any data retained across reboot.
  cache.restore();
  ESP_LOGI(TAG, "restored: size=%u", static_cast<unsigned>(cache.get_size()));

  // Start fresh for this test run.
  cache.clean();
  ESP_LOGI(TAG, "cleaned: size=%u", static_cast<unsigned>(cache.get_size()));

  // Push N_PAYLOADS test entries with distinct CO2 values.
  for (int i = 0; i < N_PAYLOADS; i++) {
    PayloadCacheType payload{};
    payload.co2.co2           = 400 + i * 100;
    payload.temp_hum_a.temperature = 20.0f + static_cast<float>(i);
    payload.temp_hum_a.humidity    = 50.0f;
    cache.push(payload);
    ESP_LOGI(TAG, "pushed[%d]: co2=%d temp=%.1f", i, payload.co2.co2,
             payload.temp_hum_a.temperature);
  }

  ESP_LOGI(TAG, "size after push: %u", static_cast<unsigned>(cache.get_size()));

  // Pop all entries and verify order (FIFO).
  bool all_ok = true;
  for (int i = 0; i < N_PAYLOADS; i++) {
    PayloadCacheType popped{};
    if (!cache.pop(popped)) {
      ESP_LOGE(TAG, "pop[%d] failed", i);
      all_ok = false;
      continue;
    }
    const int expected_co2 = 400 + i * 100;
    const bool co2_ok      = popped.co2.co2 == expected_co2;
    ESP_LOGI(TAG, "pop[%d]: co2=%d %s", i, popped.co2.co2, co2_ok ? "OK" : "FAIL");
    if (!co2_ok) {
      all_ok = false;
    }
  }

  ESP_LOGI(TAG, "size after pop: %u", static_cast<unsigned>(cache.get_size()));

  // Persist to RTC for next boot.
  cache.backup();

  ESP_LOGI(TAG, "result: %s", all_ok ? "PASS" : "FAIL");
  ESP_LOGI(TAG, "--- PayloadCache test done ---");
}
