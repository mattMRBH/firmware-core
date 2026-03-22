/**
 * AirGradient Go — Storage Service implementation
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_storage.h"

#include <cerrno>
#include <cinttypes>
#include <sys/stat.h>
#include <unistd.h>

#include "ag_log.h"

static constexpr const char *TAG = "StorageService";

// Maximum buffer size for a fully qualified route file path.
// e.g. "/nand/routes/route_12345.bin\0" — 29 chars for the default mount path;
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
    AG_LOGE(TAG, "init: NAND mount failed");
    return false;
  }

  AG_LOGI(TAG, "init: NAND mounted at %s", _nand.mount_path());
  return true;
}

// ---------------------------------------------------------------------------
// Temporary (chart) operations
// ---------------------------------------------------------------------------

void StorageService::cache_measurement(const MeasuresBasic &m) { _cache.push(m); }

bool StorageService::read_cached(uint16_t index, MeasuresBasic &out) const {
  return _cache.peek_at_index(index, out);
}

uint16_t StorageService::cached_count() const { return _cache.get_size(); }

void StorageService::backup_cache() const { _cache.backup(); }

void StorageService::restore_cache() { _cache.restore(); }

// ---------------------------------------------------------------------------
// Persistent (route) operations
// ---------------------------------------------------------------------------

bool StorageService::start_route(uint32_t session_id) {
  if (_route_file != nullptr) {
    AG_LOGW(TAG, "start_route: a route is already active");
    return false;
  }

  if (!_nand.is_mounted()) {
    AG_LOGE(TAG, "start_route: NAND not mounted");
    return false;
  }

  if (!ensure_route_dir()) {
    AG_LOGE(TAG, "start_route: failed to ensure routes directory");
    return false;
  }

  char path[MAX_PATH_LEN];
  snprintf(path, sizeof(path), "%s/routes/route_%05" PRIu32 ".bin", _nand.mount_path(), session_id);

  // Check whether a file for this session already exists (resume after sleep).
  struct stat st{};
  const bool exists = (stat(path, &st) == 0);
  const char *mode = exists ? "ab" : "wb";

  _route_file = fopen(path, mode);
  if (_route_file == nullptr) {
    AG_LOGE(TAG, "start_route: fopen failed for %s (errno=%d)", path, errno);
    return false;
  }

  // On resume, derive the existing point count from the file size so
  // current_route_point_count() reflects the full session, not just this boot.
  _current_point_count = exists ? static_cast<uint32_t>(st.st_size / sizeof(RoutePoint)) : 0U;
  _current_session_id = session_id;

  AG_LOGI(TAG, "start_route: %s %s (%" PRIu32 " points existing)", exists ? "resumed" : "opened",
          path, _current_point_count);
  return true;
}

bool StorageService::append_route_point(const RoutePoint &point) {
  if (_route_file == nullptr) {
    AG_LOGE(TAG, "append_route_point: no active route");
    return false;
  }

  const size_t written = fwrite(&point, sizeof(RoutePoint), 1, _route_file);
  if (written != 1) {
    AG_LOGE(TAG, "append_route_point: fwrite failed (errno=%d)", errno);
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

  AG_LOGI(TAG, "end_route: session %" PRIu32 " closed (%" PRIu32 " points total)",
          _current_session_id, _current_point_count);

  _current_point_count = 0;
  _current_session_id = 0;
}

bool StorageService::is_route_active() const { return _route_file != nullptr; }

uint32_t StorageService::current_route_point_count() const { return _current_point_count; }

// ---------------------------------------------------------------------------
// Private helpers
// ---------------------------------------------------------------------------

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
    AG_LOGE(TAG, "ensure_route_dir: mkdir failed for %s (errno=%d)", dir_path, errno);
    return false;
  }

  AG_LOGI(TAG, "ensure_route_dir: created %s", dir_path);
  return true;
}
