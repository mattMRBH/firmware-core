/**
 * AirGradient Go — CloudService implementation
 *
 * POST runs first each iteration; FETCH runs after.  Deadlines are
 * start-anchored so wall-clock cadence is preserved across varying
 * call durations.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_cloud.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>

#include <cJSON.h>

#include "ag_log.h"
#include "common.h"
#include "go_cloud_types.h"
#include "go_events.h"
#include "services/ag_client.h"
#include "types/wifi_types.h"
#include "go_wifi.h"

static constexpr const char *TAG = "CloudService";

// ---------------------------------------------------------------------------
// Tunables
// ---------------------------------------------------------------------------

static constexpr uint32_t CLOUD_TASK_STACK_SIZE = 8192;
static constexpr uint32_t CLOUD_TASK_PRIORITY = 4;
static constexpr size_t FETCH_BUFFER_BYTES = 2048;

/// Dashboard "no RSSI" convention; avoids a misleading 0 dB reading.
static constexpr int RSSI_UNAVAILABLE = -127;

namespace {

constexpr const char *JSON_CORRECTIONS = "corrections";
constexpr const char *JSON_PM25 = "pm02";
constexpr const char *JSON_TEMPERATURE = "atmp";
constexpr const char *JSON_HUMIDITY = "rhum";
constexpr const char *JSON_ALGORITHM = "correctionAlgorithm";
constexpr const char *JSON_SLR = "slr";
constexpr const char *JSON_INTERCEPT = "intercept";
constexpr const char *JSON_SCALING_FACTOR = "scalingFactor";
constexpr const char *JSON_SCALING_FACTOR_VIA_PM25 = "scalingFactorViaPm25";
constexpr const char *JSON_USE_EPA2021 = "useEpa2021";

bool is_json_whitespace(char value) {
  return value == ' ' || value == '\t' || value == '\n' || value == '\r';
}

bool parse_float(const cJSON *item, float &out) {
  if (!cJSON_IsNumber(item) || !std::isfinite(item->valuedouble)) {
    return false;
  }

  const float value = static_cast<float>(item->valuedouble);
  if (!std::isfinite(value)) {
    return false;
  }

  out = value;
  return true;
}

bool parse_linear_correction(const cJSON *entry, LinearCorrection &out, const char *target_name) {
  if (!cJSON_IsObject(entry)) {
    AG_LOGW(TAG, "correction %s rejected: entry is not an object", target_name);
    return false;
  }

  const cJSON *algorithm = cJSON_GetObjectItemCaseSensitive(entry, JSON_ALGORITHM);
  if (!cJSON_IsString(algorithm) || algorithm->valuestring == nullptr) {
    AG_LOGW(TAG, "correction %s rejected: algorithm is missing or not a string", target_name);
    return false;
  }

  if (std::strcmp(algorithm->valuestring, "none") == 0) {
    out = LinearCorrection{};
    return true;
  }
  if (std::strcmp(algorithm->valuestring, "custom") != 0) {
    AG_LOGW(TAG, "correction %s rejected: unsupported algorithm '%s'", target_name,
            algorithm->valuestring);
    return false;
  }

  const cJSON *slr = cJSON_GetObjectItemCaseSensitive(entry, JSON_SLR);
  if (!cJSON_IsObject(slr)) {
    AG_LOGW(TAG, "correction %s rejected: custom slr is missing or not an object", target_name);
    return false;
  }

  const cJSON *intercept = cJSON_GetObjectItemCaseSensitive(slr, JSON_INTERCEPT);
  const cJSON *scaling_factor = cJSON_GetObjectItemCaseSensitive(slr, JSON_SCALING_FACTOR);
  LinearCorrection parsed{};
  parsed.algorithm = LinearCorrectionAlgorithm::Custom;
  if (!parse_float(intercept, parsed.intercept) ||
      !parse_float(scaling_factor, parsed.scaling_factor)) {
    AG_LOGW(TAG, "correction %s rejected: custom coefficients are invalid", target_name);
    return false;
  }

  out = parsed;
  return true;
}

bool parse_pm25_correction(const cJSON *entry, Pm25Correction &out) {
  if (!cJSON_IsObject(entry)) {
    AG_LOGW(TAG, "correction %s rejected: entry is not an object", JSON_PM25);
    return false;
  }

  const cJSON *algorithm = cJSON_GetObjectItemCaseSensitive(entry, JSON_ALGORITHM);
  if (!cJSON_IsString(algorithm) || algorithm->valuestring == nullptr) {
    AG_LOGW(TAG, "correction %s rejected: algorithm is missing or not a string", JSON_PM25);
    return false;
  }

  if (std::strcmp(algorithm->valuestring, "none") == 0) {
    out = Pm25Correction{};
    return true;
  }
  if (std::strcmp(algorithm->valuestring, "epa_2021") == 0) {
    out = Pm25Correction{};
    out.algorithm = Pm25CorrectionAlgorithm::Epa2021;
    return true;
  }
  if (std::strcmp(algorithm->valuestring, "custom_via_pm25_raw") != 0) {
    AG_LOGW(TAG, "correction %s rejected: unsupported algorithm '%s'", JSON_PM25,
            algorithm->valuestring);
    return false;
  }

  const cJSON *slr = cJSON_GetObjectItemCaseSensitive(entry, JSON_SLR);
  if (!cJSON_IsObject(slr)) {
    AG_LOGW(TAG, "correction %s rejected: custom slr is missing or not an object", JSON_PM25);
    return false;
  }

  Pm25Correction parsed{};
  parsed.algorithm = Pm25CorrectionAlgorithm::CustomViaPm25Raw;
  const cJSON *intercept = cJSON_GetObjectItemCaseSensitive(slr, JSON_INTERCEPT);
  const cJSON *scaling_factor = cJSON_GetObjectItemCaseSensitive(slr, JSON_SCALING_FACTOR_VIA_PM25);
  const cJSON *use_epa2021 = cJSON_GetObjectItemCaseSensitive(slr, JSON_USE_EPA2021);
  if (!parse_float(intercept, parsed.intercept) ||
      !parse_float(scaling_factor, parsed.scaling_factor) || !cJSON_IsBool(use_epa2021)) {
    AG_LOGW(TAG, "correction %s rejected: custom parameters are invalid", JSON_PM25);
    return false;
  }
  parsed.use_epa2021 = cJSON_IsTrue(use_epa2021) != 0;

  out = parsed;
  return true;
}

GoCloudConfigUpdate parse_cloud_config(const char *buffer, size_t bytes) {
  GoCloudConfigUpdate update{};
  const char *parse_end = nullptr;
  cJSON *root = cJSON_ParseWithLengthOpts(buffer, bytes, &parse_end, 0);
  if (root == nullptr) {
    AG_LOGW(TAG, "fetch config rejected: malformed JSON");
    return update;
  }

  bool trailing_data = false;
  const char *end = buffer + bytes;
  for (const char *cursor = parse_end; cursor < end; ++cursor) {
    if (!is_json_whitespace(*cursor)) {
      trailing_data = true;
      break;
    }
  }
  if (trailing_data || !cJSON_IsObject(root)) {
    AG_LOGW(TAG, "fetch config rejected: root is invalid or has trailing data");
    cJSON_Delete(root);
    return update;
  }

  const cJSON *corrections = cJSON_GetObjectItemCaseSensitive(root, JSON_CORRECTIONS);
  if (corrections == nullptr) {
    cJSON_Delete(root);
    return update;
  }
  if (!cJSON_IsObject(corrections)) {
    AG_LOGW(TAG, "fetch config corrections rejected: value is not an object");
    cJSON_Delete(root);
    return update;
  }

  const cJSON *pm25 = cJSON_GetObjectItemCaseSensitive(corrections, JSON_PM25);
  if (pm25 != nullptr && parse_pm25_correction(pm25, update.corrections.pm25)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::Pm25Correction);
  }

  const cJSON *temperature = cJSON_GetObjectItemCaseSensitive(corrections, JSON_TEMPERATURE);
  if (temperature != nullptr &&
      parse_linear_correction(temperature, update.corrections.temperature, JSON_TEMPERATURE)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::TemperatureCorrection);
  }

  const cJSON *humidity = cJSON_GetObjectItemCaseSensitive(corrections, JSON_HUMIDITY);
  if (humidity != nullptr &&
      parse_linear_correction(humidity, update.corrections.humidity, JSON_HUMIDITY)) {
    update.update_mask |= static_cast<uint32_t>(GoConfigField::HumidityCorrection);
  }

  cJSON_Delete(root);
  return update;
}

} // namespace

// ---------------------------------------------------------------------------
// Construction / destruction
// ---------------------------------------------------------------------------

CloudService::CloudService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg)
    : _event_queue(event_queue), _client(deps.client), _wifi(deps.wifi), _cfg(cfg),
      _disable_cloud(cfg.disable_cloud) {}

CloudService::~CloudService() { stop(); }

// ---------------------------------------------------------------------------
// Lifecycle
// ---------------------------------------------------------------------------

bool CloudService::start() {
  if (_task_handle != nullptr) {
    return true;
  }

  log_heap(TAG, "cloud.start:enter");
  _shutdown_pending.store(false);

  _fetch_buf = static_cast<char *>(std::malloc(FETCH_BUFFER_BYTES));
  if (_fetch_buf == nullptr) {
    AG_LOGE(TAG, "start: fetch buffer alloc failed (%zu bytes)", FETCH_BUFFER_BYTES);
    return false;
  }

  // Semaphore must exist before task_create so stop() never races.
  if (!_done_sem.create()) {
    AG_LOGE(TAG, "start: semaphore creation failed");
    std::free(_fetch_buf);
    _fetch_buf = nullptr;
    return false;
  }

  const bool ok = RTOS::task_create(_task_entry, "cloud", CLOUD_TASK_STACK_SIZE, this,
                                    CLOUD_TASK_PRIORITY, &_task_handle);
  if (!ok) {
    AG_LOGE(TAG, "start: task_create failed");
    _done_sem.destroy();
    std::free(_fetch_buf);
    _fetch_buf = nullptr;
    _task_handle = nullptr;
    return false;
  }

  AG_LOGI(TAG, "start: task spawned (stack=%lu, prio=%lu, fetch_buf=%zu)",
          static_cast<unsigned long>(CLOUD_TASK_STACK_SIZE),
          static_cast<unsigned long>(CLOUD_TASK_PRIORITY), FETCH_BUFFER_BYTES);
  log_heap(TAG, "cloud.start:exit");
  return true;
}

void CloudService::stop() {
  if (_task_handle == nullptr) {
    return;
  }

  log_heap(TAG, "cloud.stop:enter");
  AG_LOGI(TAG, "stop: latching shutdown");
  _shutdown_pending.store(true);
  _wake();

  // Bounded by one WifiHttpClient timeout (~15 s).
  _done_sem.take();

  // Task is parked on notify-wait; safe to delete externally.
  RTOS::task_delete(_task_handle);
  _task_handle = nullptr;

  if (_fetch_buf != nullptr) {
    std::free(_fetch_buf);
    _fetch_buf = nullptr;
  }
  _done_sem.destroy();

  _shutdown_pending.store(false);
  _post_due = 0;
  _fetch_due = 0;
  _was_armed = false;
  log_heap(TAG, "cloud.stop:exit");
}

// ---------------------------------------------------------------------------
// State actions
// ---------------------------------------------------------------------------

void CloudService::arm(bool fire_now) {
  if (fire_now) {
    _fire_now_pending.store(true);
  }
  _armed.store(true);
  _wake();
}

void CloudService::disarm() {
  _armed.store(false);
  _wake();
}

void CloudService::set_disable_cloud(bool disable) {
  _disable_cloud.store(disable);
  _wake();
}

void CloudService::update_measures_snapshot(const MeasuresAGo &m) {
  _snapshot_mtx.lock();
  _latest_snapshot = m;
  _snapshot_mtx.unlock();
}

// ---------------------------------------------------------------------------
// Task entry / loop
// ---------------------------------------------------------------------------

// static
void CloudService::_task_entry(void *arg) {
  static_cast<CloudService *>(arg)->_run();
  // Park forever — stop() deletes the task externally.
  while (true) {
    RTOS::delay_ms(60000);
  }
}

void CloudService::_run() {
  for (;;) {
    const uint32_t now = static_cast<uint32_t>(RTOS::get_time_ms());
    const uint32_t wake_ms = _run_iteration(now);

    if (_shutdown_pending.load()) {
      // Park until stop() calls task_delete (avoids double-delete race).
      RTOS::task_notify_take(UINT32_MAX);
      return;
    }

    if (wake_ms > 0) {
      RTOS::task_notify_take(wake_ms);
    }
  }
}

uint32_t CloudService::_run_iteration(uint32_t now) {
  if (_shutdown_pending.load()) {
#ifdef TEST_HOST
    _test_done_signal_count += 1;
#endif
    _done_sem.give();
    return UINT32_MAX;
  }

  const bool armed = _armed.load();
  const bool disable = _disable_cloud.load();

  // Handle Disarmed→Armed transition: snap deadlines to now (fire_now)
  // or one interval into the future.
  if (armed != _was_armed) {
    if (armed) {
      const bool fire_now = _fire_now_pending.exchange(false);
      _post_due = fire_now ? now : now + _cfg.post_interval_ms;
      _fetch_due = fire_now ? now : now + _cfg.fetch_interval_ms;
    }
    _was_armed = armed;
  }

  if (!armed || disable) {
    // Slide expired deadlines forward while disabled so re-enable
    // doesn't catch up on missed intervals.
    if (armed) {
      if (static_cast<int32_t>(now - _post_due) >= 0) {
        _post_due = now + _cfg.post_interval_ms;
      }
      if (static_cast<int32_t>(now - _fetch_due) >= 0) {
        _fetch_due = now + _cfg.fetch_interval_ms;
      }
    }
    if (!armed) {
      return UINT32_MAX;
    }
    const uint32_t next = std::min(_post_due, _fetch_due);
    const int64_t delta = static_cast<int64_t>(next) - static_cast<int64_t>(now);
    return delta > 0 ? static_cast<uint32_t>(delta) : 0;
  }

  // POST priority: fire POST first; return 0 to re-sample state before
  // considering FETCH (gates out disarm/disable arriving mid-POST).
  if (static_cast<int32_t>(now - _post_due) >= 0) {
    _do_post(now);
    return 0;
  }

  if (static_cast<int32_t>(now - _fetch_due) >= 0) {
    _do_fetch(now);
    return 0;
  }

  const uint32_t next = std::min(_post_due, _fetch_due);
  const int64_t delta = static_cast<int64_t>(next) - static_cast<int64_t>(now);
  return delta > 0 ? static_cast<uint32_t>(delta) : 0;
}

// ---------------------------------------------------------------------------
// HTTP legs
// ---------------------------------------------------------------------------

void CloudService::_do_post(uint32_t now_ms) {
  const uint32_t post_started_at = now_ms;
  MeasuresAGo snap = _snapshot_copy();

  const int raw_rssi = _wifi.rssi();
  const int rssi = (raw_rssi == WIFI_RSSI_INVALID) ? RSSI_UNAVAILABLE : raw_rssi;

  log_heap(TAG, "cloud.post:pre-tls");
  const AgClientResult result = _client.http_post_measures(snap, rssi);
  log_heap(TAG, "cloud.post:post-tls");
  AG_LOGI(TAG, "post_measures result=%d rssi=%d", static_cast<int>(result), rssi);

  Event evt{};
  evt.type = EventType::PostMeasuresResult;
  evt.cloud_result = static_cast<CloudResultByte>(result);
  RTOS::queue_send(_event_queue, &evt);

  // Anchor to start time, not completion.
  _post_due = post_started_at + _cfg.post_interval_ms;
}

void CloudService::_do_fetch(uint32_t now_ms) {
  const uint32_t fetch_started_at = now_ms;
  size_t bytes = 0;
  log_heap(TAG, "cloud.fetch:pre-tls");
  const AgClientResult result = _client.http_fetch_config(_fetch_buf, FETCH_BUFFER_BYTES, &bytes);
  log_heap(TAG, "cloud.fetch:post-tls");

  const size_t logged = bytes < FETCH_BUFFER_BYTES ? bytes : FETCH_BUFFER_BYTES;
  AG_LOGI(TAG, "fetch_config result=%d bytes=%zu body=%.*s", static_cast<int>(result), bytes,
          static_cast<int>(logged), _fetch_buf != nullptr ? _fetch_buf : "");

  GoCloudConfigUpdate update{};
  if (result == AgClientResult::Ok && _fetch_buf != nullptr && bytes < FETCH_BUFFER_BYTES) {
    update = parse_cloud_config(_fetch_buf, bytes);
  }

  Event evt{};
  evt.type = EventType::FetchConfigResult;
  evt.fetch_config.result = static_cast<CloudResultByte>(result);
  evt.fetch_config.update = update;
  RTOS::queue_send(_event_queue, &evt);

  _fetch_due = fetch_started_at + _cfg.fetch_interval_ms;
}

// ---------------------------------------------------------------------------
// Helpers
// ---------------------------------------------------------------------------

MeasuresAGo CloudService::_snapshot_copy() {
  _snapshot_mtx.lock();
  MeasuresAGo out = _latest_snapshot;
  _snapshot_mtx.unlock();
  return out;
}

void CloudService::_wake() {
  if (_task_handle == nullptr) {
    return;
  }
  RTOS::task_notify_send(_task_handle, 0);
}
