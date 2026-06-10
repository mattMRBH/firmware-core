/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_WIFI_HTTP_OTA_SOURCE_H
#define AG_WIFI_HTTP_OTA_SOURCE_H

#include <cstddef>
#include <cstdint>

#include "hal/ota_image_source.h"
#include "types/ota_types.h"

// Pull in the Kconfig URL-buffer size when building with ESP-IDF; fall back to
// the spec default for native builds where no sdkconfig.h exists.
#if defined(__has_include) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#ifndef CONFIG_AG_OTA_URL_BUFFER_SIZE
#define CONFIG_AG_OTA_URL_BUFFER_SIZE 256
#endif

// WiFi pull source: streams a single HTTP GET through esp_http_client straight
// into the writer - no whole-image buffer. Firmware build only; all
// esp_http_client includes live behind #ifndef TEST_HOST in the .cpp, and the
// client handle is held as an opaque void *.
class WifiHttpOtaSource : public OtaImageSource {
public:
  // Copies the bounded request fields into internal buffers and builds the URL
  // at construction; the OtaRequest (and its strings) need not outlive this
  // call. If the URL build fails (missing field / truncation / unknown model),
  // construction stores the failure and open() reports it.
  explicit WifiHttpOtaSource(const OtaRequest &request);
  ~WifiHttpOtaSource() override;

  OtaStatus open(size_t *out_total_size) override;  // returns _init_status on failure
  int read(uint8_t *buf, size_t buf_size) override; // esp_http_client_read
  void close() override;                            // close + cleanup (idempotent)

private:
  char _url[CONFIG_AG_OTA_URL_BUFFER_SIZE]; // built once at construction
  OtaStatus _init_status;                   // Ok, or InvalidArgument if build failed
  void *_client = nullptr;                  // opaque esp_http_client_handle_t (cast in .cpp)
};

#endif // AG_WIFI_HTTP_OTA_SOURCE_H
