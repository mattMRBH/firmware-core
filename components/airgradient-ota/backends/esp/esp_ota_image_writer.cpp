/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "backends/esp/esp_ota_image_writer.h"

#include "ag_log.h"

#ifndef TEST_HOST
#include "esp_ota_ops.h"
#include "esp_partition.h"
#endif

namespace {
constexpr const char *TAG = "EspOtaImageWriter";
} // namespace

EspOtaImageWriter::~EspOtaImageWriter() { abort(); }

OtaStatus EspOtaImageWriter::begin(size_t total_size) {
#ifndef TEST_HOST
  if (_in_progress) {
    abort();
  }

  const esp_partition_t *partition = esp_ota_get_next_update_partition(nullptr);
  if (partition == nullptr) {
    AG_LOGE(TAG, "no passive OTA partition found");
    return OtaStatus::FlashError;
  }

  esp_ota_handle_t handle = 0;
  const size_t image_size = (total_size == 0) ? OTA_SIZE_UNKNOWN : total_size;
  const esp_err_t err = esp_ota_begin(partition, image_size, &handle);
  if (err != ESP_OK) {
    AG_LOGE(TAG, "esp_ota_begin failed: %s", esp_err_to_name(err));
    return OtaStatus::FlashError;
  }

  _partition = partition;
  _handle = handle;
  _in_progress = true;
  _bytes_written = 0;
  return OtaStatus::Ok;
#else
  (void)total_size;
  return OtaStatus::FlashError;
#endif
}

OtaStatus EspOtaImageWriter::write(const uint8_t *data, size_t len) {
  if (data == nullptr || len == 0) {
    return OtaStatus::InvalidArgument;
  }

#ifndef TEST_HOST
  if (!_in_progress) {
    return OtaStatus::FlashError;
  }

  const esp_err_t err = esp_ota_write(static_cast<esp_ota_handle_t>(_handle), data, len);
  if (err != ESP_OK) {
    AG_LOGW(TAG, "esp_ota_write failed: %s", esp_err_to_name(err));
    return OtaStatus::FlashError;
  }

  _bytes_written += len;
  return OtaStatus::Ok;
#else
  return OtaStatus::FlashError;
#endif
}

OtaStatus EspOtaImageWriter::finish() {
#ifndef TEST_HOST
  if (!_in_progress) {
    return OtaStatus::FlashError;
  }

  const esp_err_t end_err = esp_ota_end(static_cast<esp_ota_handle_t>(_handle));
  _in_progress = false;
  if (end_err != ESP_OK) {
    if (end_err == ESP_ERR_OTA_VALIDATE_FAILED) {
      AG_LOGE(TAG, "image validation failed");
      return OtaStatus::InvalidImage;
    }
    AG_LOGE(TAG, "esp_ota_end failed: %s", esp_err_to_name(end_err));
    return OtaStatus::FlashError;
  }

  const esp_err_t boot_err =
      esp_ota_set_boot_partition(static_cast<const esp_partition_t *>(_partition));
  if (boot_err != ESP_OK) {
    AG_LOGE(TAG, "esp_ota_set_boot_partition failed: %s", esp_err_to_name(boot_err));
    return OtaStatus::FlashError;
  }

  AG_LOGI(TAG, "OTA image applied; reboot to run it");
  return OtaStatus::Ok;
#else
  return OtaStatus::FlashError;
#endif
}

void EspOtaImageWriter::abort() {
#ifndef TEST_HOST
  if (!_in_progress) {
    return;
  }
  esp_ota_abort(static_cast<esp_ota_handle_t>(_handle));
  _in_progress = false;
  AG_LOGI(TAG, "OTA aborted");
#endif
}

size_t EspOtaImageWriter::bytes_written() const { return _bytes_written; }
