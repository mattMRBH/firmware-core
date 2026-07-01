/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_ESP_OTA_IMAGE_WRITER_H
#define AG_ESP_OTA_IMAGE_WRITER_H

#include <cstddef>
#include <cstdint>

#include "hal/ota_image_writer.h"

// Concrete OtaImageWriter over esp_ota_ops. Firmware build only; all ESP-IDF
// includes live behind #ifndef TEST_HOST in the .cpp. The partition handle is
// held as an opaque void * and cast inside the .cpp so this header stays free
// of ESP-IDF types.
class EspOtaImageWriter : public OtaImageWriter {
public:
  EspOtaImageWriter() = default;
  ~EspOtaImageWriter() override;

  OtaStatus begin(size_t total_size) override;
  OtaStatus write(const uint8_t *data, size_t len) override;
  OtaStatus finish() override;
  void abort() override;
  size_t bytes_written() const override;

private:
  const void *_partition = nullptr; // const esp_partition_t *
  uint64_t _handle = 0;             // esp_ota_handle_t
  bool _in_progress = false;
  size_t _bytes_written = 0;
};

#endif // AG_ESP_OTA_IMAGE_WRITER_H
