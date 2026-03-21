#include "test_gpio.h"

#include "esp_log.h"

#include "board_config.h"
#include "rtos.h"

static constexpr const char *TAG = "test_gpio";

static constexpr int BLINK_COUNT = 5;
static constexpr int BLINK_INTERVAL_MS = 200;

void run_test_gpio(const gpio::Hal &hal) {
  ESP_LOGI(TAG, "--- GPIO test start ---");

  if (!hal.configure(static_cast<int>(PIN_LED_INDICATOR), gpio::Mode::Output,
                     gpio::PullMode::Floating, gpio::InterruptType::Disabled)) {
    ESP_LOGE(TAG, "configure LED pin failed");
    return;
  }

  if (!hal.configure(static_cast<int>(PIN_BOOT_BUTTON), gpio::Mode::Input, gpio::PullMode::Floating,
                     gpio::InterruptType::Disabled)) {
    ESP_LOGE(TAG, "configure boot button pin failed");
    return;
  }

  ESP_LOGI(TAG, "blinking LED %d times", BLINK_COUNT);
  for (int i = 0; i < BLINK_COUNT; i++) {
    hal.set_level(static_cast<int>(PIN_LED_INDICATOR), 1);
    RTOS::delay_ms(BLINK_INTERVAL_MS);
    hal.set_level(static_cast<int>(PIN_LED_INDICATOR), 0);
    RTOS::delay_ms(BLINK_INTERVAL_MS);
  }

  ESP_LOGI(TAG, "boot button level: %d", hal.get_level(static_cast<int>(PIN_BOOT_BUTTON)));

  ESP_LOGI(TAG, "--- GPIO test done ---");
}
