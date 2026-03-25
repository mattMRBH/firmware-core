/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "common.h"

#include "rtos.h"

#ifndef TEST_HOST
#include "esp_app_desc.h"
#include "esp_mac.h"
#include "esp_system.h"
#endif

static constexpr uint32_t EXT_WDT_PULSE_MS = 20;

bool ext_watchdog_init(const gpio::Hal &hal, int pin) {
  return hal.configure(pin, gpio::Mode::Output, gpio::PullMode::Floating,
                       gpio::InterruptType::Disabled);
}

void ext_watchdog_reset(const gpio::Hal &hal, int pin) {
  hal.set_level(pin, 1);
  RTOS::delay_ms(EXT_WDT_PULSE_MS);
  hal.set_level(pin, 0);
}

std::string build_serial_number() {
#ifndef TEST_HOST
  uint8_t mac_address[6];
  esp_err_t err = esp_read_mac(mac_address, ESP_MAC_WIFI_STA);
  if (err != ESP_OK) {
    printf("Failed to get MAC address (%s)\n", esp_err_to_name(err));
    return {};
  }

  char result[13] = {0};
  snprintf(result, sizeof(result), "%02x%02x%02x%02x%02x%02x", mac_address[0], mac_address[1],
           mac_address[2], mac_address[3], mac_address[4], mac_address[5]);
  return std::string(result);
#else
  return {};
#endif
}

std::string build_firmware_version() {
#ifndef TEST_HOST
  const esp_app_desc_t *desc = esp_app_get_description();
  if (desc == nullptr || desc->version[0] == '\0') {
    return {};
  }

  return std::string(desc->version);
#else
  return {};
#endif
}

void reboot() {
#ifndef TEST_HOST
  esp_restart();
#endif
}
