/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "services/ota_updater.h"

#include <cstdint>

#include "ag_log.h"
#include "rtos.h"

// Pull in Kconfig values when building with ESP-IDF; fall back to the spec
// defaults for native host-test builds where no sdkconfig.h exists.
#if defined(__has_include) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#ifndef CONFIG_AG_OTA_READ_BUFFER_SIZE
#define CONFIG_AG_OTA_READ_BUFFER_SIZE 1024
#endif

#ifndef CONFIG_AG_OTA_PROGRESS_INTERVAL_MS
#define CONFIG_AG_OTA_PROGRESS_INTERVAL_MS 250
#endif

namespace {
constexpr const char *TAG = "OtaUpdater";
} // namespace

OtaUpdater::OtaUpdater(OtaImageSource &source, OtaImageWriter &writer)
    : _source(source), _writer(writer), _on_progress(nullptr) {}

void OtaUpdater::set_on_progress(OtaProgressCallback cb) { _on_progress = cb; }

void OtaUpdater::_emit_progress(OtaState state, size_t total_size) {
  if (_on_progress == nullptr) {
    return;
  }

  const size_t written = _writer.bytes_written();
  uint8_t percent = 0;
  if (total_size > 0) {
    size_t value = (written * 100) / total_size;
    if (value > 100) {
      value = 100;
    }
    percent = static_cast<uint8_t>(value);
  }

  OtaProgress progress{state, written, total_size, percent};
  _on_progress(progress);
}

OtaStatus OtaUpdater::run() {
  _emit_progress(OtaState::Checking, 0);

  size_t total = 0;
  OtaStatus st = _source.open(&total);
  if (st != OtaStatus::Ok) { // UpToDate / Declined / error
    _source.close();         // always close after open()
    // UpToDate and Declined are non-update outcomes, not failures.
    const bool skipped = (st == OtaStatus::UpToDate || st == OtaStatus::Declined);
    _emit_progress(skipped ? OtaState::Skipped : OtaState::Failed, total);
    return st;
  }

  st = _writer.begin(total);
  if (st != OtaStatus::Ok) {
    _source.close();
    _emit_progress(OtaState::Failed, total);
    return st;
  }

  // Emit one immediate Downloading so small images still report progress.
  _emit_progress(OtaState::Downloading, total);

  uint8_t buf[CONFIG_AG_OTA_READ_BUFFER_SIZE];
  uint64_t last_cb = RTOS::get_time_ms();
  while (true) {
    const int n = _source.read(buf, sizeof(buf));
    if (n == 0) {
      break; // EOF
    }
    if (n < 0) {
      _writer.abort();
      _source.close();
      AG_LOGE(TAG, "read failed");
      _emit_progress(OtaState::Failed, total);
      return OtaStatus::TransportError;
    }

    st = _writer.write(buf, static_cast<size_t>(n));
    if (st != OtaStatus::Ok) {
      _writer.abort();
      _source.close();
      AG_LOGE(TAG, "write failed");
      _emit_progress(OtaState::Failed, total);
      return st;
    }

    const uint64_t now = RTOS::get_time_ms();
    if (now - last_cb >= CONFIG_AG_OTA_PROGRESS_INTERVAL_MS) {
      _emit_progress(OtaState::Downloading, total);
      last_cb = now;
    }
  }

  _source.close();

  // Guard against a truncated download when the total size is known.
  if (total > 0 && _writer.bytes_written() != total) {
    _writer.abort();
    AG_LOGE(TAG, "truncated download: %u of %u bytes", (unsigned)_writer.bytes_written(),
            (unsigned)total);
    _emit_progress(OtaState::Failed, total);
    return OtaStatus::TransportError;
  }

  _emit_progress(OtaState::Applying, total);
  AG_LOGI(TAG, "Applying new firmware");
  st = _writer.finish();
  _emit_progress(st == OtaStatus::Ok ? OtaState::Done : OtaState::Failed, total);
  return st;
}
