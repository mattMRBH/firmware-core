#include "test_ota.h"

#include <cstring>

#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "sdkconfig.h"

#include "backends/esp/esp_ota_image_writer.h"
#include "backends/wifi/wifi_http_ota_source.h"
#include "services/ota_updater.h"
#include "types/ota_types.h"

static constexpr const char *TAG = "test_ota";

// ---- Edit these before flashing --------------------------------------
// Do not commit real credentials or a real device serial number.
static constexpr const char *WIFI_SSID = "";
static constexpr const char *WIFI_PASSWORD = "";
// 12-char hex; must be registered on HTTP_DOMAIN for the apply case.
static constexpr const char *SERIAL_NUMBER = "aabbccddeeff";
// Reported as the running firmware. Set to the server's latest version to
// observe the UpToDate (304) path; set lower than latest to fetch an image.
static constexpr const char *CURRENT_FIRMWARE = "3.6.0";
// Empty -> use compiled-in "hw.airgradient.com".
static constexpr const char *HTTP_DOMAIN = "hw.airgradient.com";
// Device model -> URL shape mapping (OneOpenAir or Max).
static constexpr OtaDeviceModel DEVICE_MODEL = OtaDeviceModel::OneOpenAir;
// ----------------------------------------------------------------------

static constexpr int WIFI_CONNECTED_BIT = BIT0;
static constexpr int WIFI_FAILED_BIT = BIT1;
static constexpr int WIFI_MAX_RETRIES = 5;

namespace {

EventGroupHandle_t s_wifi_event_group = nullptr;
int s_retry_count = 0;

void on_wifi_event(void * /*arg*/, esp_event_base_t event_base, int32_t event_id,
                   void * /*event_data*/) {
  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
    return;
  }

  if (event_base == WIFI_EVENT && event_id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_retry_count < WIFI_MAX_RETRIES) {
      ++s_retry_count;
      ESP_LOGW(TAG, "WiFi disconnect, retry %d/%d", s_retry_count, WIFI_MAX_RETRIES);
      esp_wifi_connect();
    } else {
      ESP_LOGE(TAG, "WiFi connection failed after %d retries", WIFI_MAX_RETRIES);
      xEventGroupSetBits(s_wifi_event_group, WIFI_FAILED_BIT);
    }
  }
}

void on_ip_event(void * /*arg*/, esp_event_base_t event_base, int32_t event_id, void *event_data) {
  if (event_base == IP_EVENT && event_id == IP_EVENT_STA_GOT_IP) {
    auto *event = static_cast<ip_event_got_ip_t *>(event_data);
    ESP_LOGI(TAG, "Got IP: " IPSTR, IP2STR(&event->ip_info.ip));
    s_retry_count = 0;
    xEventGroupSetBits(s_wifi_event_group, WIFI_CONNECTED_BIT);
  }
}

// Blocks until DHCP IP arrives or retry budget runs out.
bool connect_wifi(const char *ssid, const char *password) {
  s_wifi_event_group = xEventGroupCreate();
  if (s_wifi_event_group == nullptr) {
    ESP_LOGE(TAG, "xEventGroupCreate failed");
    return false;
  }

  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  esp_event_handler_instance_t any_wifi = nullptr;
  esp_event_handler_instance_t got_ip = nullptr;
  ESP_ERROR_CHECK(esp_event_handler_instance_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &on_wifi_event,
                                                      nullptr, &any_wifi));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &on_ip_event,
                                                      nullptr, &got_ip));

  wifi_config_t wifi_config = {};
  std::strncpy(reinterpret_cast<char *>(wifi_config.sta.ssid), ssid,
               sizeof(wifi_config.sta.ssid) - 1);
  std::strncpy(reinterpret_cast<char *>(wifi_config.sta.password), password,
               sizeof(wifi_config.sta.password) - 1);

  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "Connecting to SSID '%s' ...", ssid);
  const EventBits_t bits =
      xEventGroupWaitBits(s_wifi_event_group, WIFI_CONNECTED_BIT | WIFI_FAILED_BIT,
                          /*xClearOnExit=*/pdFALSE, /*xWaitForAllBits=*/pdFALSE, portMAX_DELAY);

  return (bits & WIFI_CONNECTED_BIT) != 0;
}

const char *status_to_str(OtaStatus s) {
  switch (s) {
  case OtaStatus::Ok:
    return "Ok";
  case OtaStatus::UpToDate:
    return "UpToDate";
  case OtaStatus::Declined:
    return "Declined";
  case OtaStatus::TransportError:
    return "TransportError";
  case OtaStatus::ServerError:
    return "ServerError";
  case OtaStatus::FlashError:
    return "FlashError";
  case OtaStatus::InvalidImage:
    return "InvalidImage";
  case OtaStatus::InvalidArgument:
    return "InvalidArgument";
  }
  return "?";
}

const char *state_to_str(OtaState s) {
  switch (s) {
  case OtaState::Idle:
    return "Idle";
  case OtaState::Checking:
    return "Checking";
  case OtaState::Downloading:
    return "Downloading";
  case OtaState::Applying:
    return "Applying";
  case OtaState::Done:
    return "Done";
  case OtaState::Skipped:
    return "Skipped";
  case OtaState::Failed:
    return "Failed";
  }
  return "?";
}

struct TestCase {
  const char *name;
  const char *serial_number;
  const char *current_firmware;
  const char *domain_override; // "" -> keep file-level HTTP_DOMAIN
  OtaStatus expected;
};

bool run_case(const TestCase &tc) {
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "==== Case: %s ====", tc.name);
  ESP_LOGI(TAG, "  sn='%s' fw='%s' domain_override='%s'", tc.serial_number, tc.current_firmware,
           tc.domain_override);
  ESP_LOGI(TAG, "  expect=%s", status_to_str(tc.expected));

  const char *domain = (tc.domain_override[0] != '\0') ? tc.domain_override : HTTP_DOMAIN;
  OtaRequest req{tc.serial_number, tc.current_firmware, domain, DEVICE_MODEL};

  WifiHttpOtaSource source(req);
  EspOtaImageWriter writer;
  OtaUpdater updater(source, writer);
  updater.set_on_progress([](const OtaProgress &p) {
    ESP_LOGI(TAG, "  progress: state=%s %u%% (%u bytes)", state_to_str(p.state), p.percent,
             static_cast<unsigned>(p.bytes_written));
  });

  const OtaStatus st = updater.run();
  const bool match = (st == tc.expected);
  ESP_LOGI(TAG, "  result: got=%s expect=%s  %s", status_to_str(st), status_to_str(tc.expected),
           match ? "[PASS]" : "[FAIL]");
  if (st == OtaStatus::Ok) {
    ESP_LOGW(TAG, "  image staged on the next boot partition; reboot to run it");
  }

  ESP_LOGI(TAG, "==== Case %s: %s ====", tc.name, match ? "PASS" : "FAIL");
  return match;
}

} // namespace

void run_test_ota() {
  ESP_LOGI(TAG, "--- OTA smoke test start ---");
  ESP_LOGI(TAG, "Config: ssid='%s' sn='%s' fw='%s' domain='%s'", WIFI_SSID, SERIAL_NUMBER,
           CURRENT_FIRMWARE, HTTP_DOMAIN);

  if (!connect_wifi(WIFI_SSID, WIFI_PASSWORD)) {
    ESP_LOGE(TAG, "[FAIL] WiFi connect");
    return;
  }
  ESP_LOGI(TAG, "[PASS] WiFi connected");

  // Non-applying cases only by default. The "up to date" expectation assumes
  // CURRENT_FIRMWARE matches the server's latest build (304); adjust per the
  // target server. The DNS-failure case is deterministic.
  static const TestCase safe_cases[] = {
      {
          "up to date (304 probe)",
          SERIAL_NUMBER,
          CURRENT_FIRMWARE,
          "",
          OtaStatus::UpToDate,
      },
      {
          "wrong endpoint (DNS failure)",
          SERIAL_NUMBER,
          CURRENT_FIRMWARE,
          "no-such-host.airgradient-test.invalid",
          OtaStatus::TransportError,
      },
  };

  size_t passed = 0;
  size_t total = 0;
  for (const auto &tc : safe_cases) {
    ++total;
    if (run_case(tc)) {
      ++passed;
    }
  }

#ifdef TEST_OTA_ALLOW_APPLY
  // Download-and-apply path. Expects an image to be served for SERIAL_NUMBER at
  // CURRENT_FIRMWARE; on Ok the boot partition is set but we do NOT reboot.
  static const TestCase apply_case = {
      "apply latest image", SERIAL_NUMBER, CURRENT_FIRMWARE, "", OtaStatus::Ok,
  };
  ++total;
  if (run_case(apply_case)) {
    ++passed;
  }
#else
  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "apply case skipped (build with -DTEST_OTA_ALLOW_APPLY to enable)");
#endif

  ESP_LOGI(TAG, "");
  ESP_LOGI(TAG, "--- Summary: %zu/%zu cases passed ---", passed, total);
  ESP_LOGI(TAG, "--- OTA smoke test done ---");
}
