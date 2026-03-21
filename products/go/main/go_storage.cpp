/**
 * AirGradient Go — Storage Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

// Under TEST_HOST the ESP-IDF logging headers are not available.
// Stub them out so the translation unit compiles for native host tests.
#ifdef TEST_HOST
#define ESP_LOGI(tag, ...)                                                                         \
  do {                                                                                             \
  } while (0)
#define ESP_LOGW(tag, ...)                                                                         \
  do {                                                                                             \
  } while (0)
#define ESP_LOGE(tag, ...)                                                                         \
  do {                                                                                             \
  } while (0)
#else
#include "esp_log.h"
#endif

#include "go_storage.h"

#include <cerrno>
#include <dirent.h>
#include <sys/stat.h>
#include <unistd.h>

static constexpr const char *TAG = "StorageService";

// Maximum buffer size for a fully qualified route file path.
// e.g. "/nand/routes/route_9999.bin\0" — 28 chars for the default mount path;
// 256 provides ample headroom for any reasonable mount path.
static constexpr size_t MAX_PATH_LEN = 256;

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

StorageService::StorageService(PayloadCache &cache, NandStorage &nand)
    : _cache(cache), _nand(nand) {}

// ---------------------------------------------------------------------------
// Initialization
// ---------------------------------------------------------------------------

bool StorageService::init() {
  if (!_nand.init()) {
    ESP_LOGE(TAG, "init: NAND mount failed");
    return false;
  }

  _next_route_index = scan_route_index();
  ESP_LOGI(TAG, "init: NAND mounted at %s, next route index = %u", _nand.mount_path(),
           _next_route_index);
  return true;
}

// ---------------------------------------------------------------------------
// Temporary (chart) operations
// ---------------------------------------------------------------------------

void StorageService::cache_measurement(const Measures &m) {
  // PayloadCacheType must be Measures (CONFIG_PAYLOAD_CACHE_TYPE_FULL, the
  // default). If CONFIG_PAYLOAD_CACHE_TYPE_BASIC is set, this call will not
  // compile and a conversion layer would be needed.
  _cache.push(m);
}

bool StorageService::read_cached(uint16_t index, Measures &out) const {
  return _cache.peek_at_index(index, out);
}

uint16_t StorageService::cached_count() const { return _cache.get_size(); }

void StorageService::backup_cache() const { _cache.backup(); }

void StorageService::restore_cache() { _cache.restore(); }

// ---------------------------------------------------------------------------
// Persistent (route) operations
// ---------------------------------------------------------------------------

bool StorageService::start_route() {
  if (_route_file != nullptr) {
    ESP_LOGW(TAG, "start_route: a route is already active");
    return false;
  }

  if (!_nand.is_mounted()) {
    ESP_LOGE(TAG, "start_route: NAND not mounted");
    return false;
  }

  if (!ensure_route_dir()) {
    ESP_LOGE(TAG, "start_route: failed to ensure routes directory");
    return false;
  }

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "%s/routes/route_%04u.bin", _nand.mount_path(), _next_route_index);

  _route_file = fopen(path, "wb");
  if (_route_file == nullptr) {
    ESP_LOGE(TAG, "start_route: fopen failed for %s (errno=%d)", path, errno);
    return false;
  }

  _current_point_count = 0;
  ESP_LOGI(TAG, "start_route: opened %s", path);
  return true;
}

bool StorageService::append_route_point(const RoutePoint &point) {
  if (_route_file == nullptr) {
    ESP_LOGE(TAG, "append_route_point: no active route");
    return false;
  }

  const size_t written = fwrite(&point, sizeof(RoutePoint), 1, _route_file);
  if (written != 1) {
    ESP_LOGE(TAG, "append_route_point: fwrite failed (errno=%d)", errno);
    return false;
  }

  _current_point_count++;
  return true;
}

void StorageService::end_route() {
  if (_route_file == nullptr) {
    return;
  }

  // Flush the stdio buffer to the OS, then fsync to NAND to minimize data loss
  // on unexpected power cut.
  fflush(_route_file);
  fsync(fileno(_route_file));
  fclose(_route_file);
  _route_file = nullptr;

  ESP_LOGI(TAG, "end_route: route %u closed (%lu points)", _next_route_index,
           static_cast<unsigned long>(_current_point_count));

  _current_point_count = 0;
  _next_route_index++;
}

bool StorageService::is_route_active() const { return _route_file != nullptr; }

uint32_t StorageService::current_route_point_count() const { return _current_point_count; }

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

uint16_t StorageService::scan_route_index() const {
  if (!_nand.is_mounted()) {
    return 1;
  }

  char dir_path[MAX_PATH_LEN];
  snprintf(dir_path, sizeof(dir_path), "%s/routes", _nand.mount_path());

  DIR *dir = opendir(dir_path);
  if (dir == nullptr) {
    // Routes directory does not exist yet — first route will be index 1.
    return 1;
  }

  uint16_t max_index = 0;
  struct dirent *entry = nullptr;
  while ((entry = readdir(dir)) != nullptr) {
    unsigned int idx = 0;
    // Use %u (no width) in sscanf so files with any digit count are matched.
    if (sscanf(entry->d_name, "route_%u.bin", &idx) == 1) {
      if (idx > max_index) {
        max_index = static_cast<uint16_t>(idx);
      }
    }
  }
  closedir(dir);

  // Next available index is one past the highest found (minimum 1).
  return static_cast<uint16_t>(max_index + 1U);
}

bool StorageService::ensure_route_dir() const {
  if (!_nand.is_mounted()) {
    return false;
  }

  char dir_path[MAX_PATH_LEN];
  snprintf(dir_path, sizeof(dir_path), "%s/routes", _nand.mount_path());

  struct stat st{};
  if (stat(dir_path, &st) == 0) {
    // Path already exists — assume it is a directory.
    return true;
  }

  // Directory does not exist; create it.
  if (mkdir(dir_path, 0755) != 0) {
    ESP_LOGE(TAG, "ensure_route_dir: mkdir failed for %s (errno=%d)", dir_path, errno);
    return false;
  }

  ESP_LOGI(TAG, "ensure_route_dir: created %s", dir_path);
  return true;
}
