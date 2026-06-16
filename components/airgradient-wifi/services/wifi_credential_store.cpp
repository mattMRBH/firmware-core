/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "wifi_credential_store.h"

#include <cstdio>
#include <cstring>
#include <string>

#include "config_store.h"

namespace {

constexpr const char *KEY_COUNT = "count";
constexpr const char *KEY_SSID_PREFIX = "ssid";
constexpr const char *KEY_PW_PREFIX = "pw";

// Fixed buffer capacities (NUL-terminated): SSID 32 + 1, password 63 + 1.
constexpr size_t SSID_MAX_LEN = sizeof(WifiCredential::ssid) - 1;   // 32
constexpr size_t PW_MAX_LEN = sizeof(WifiCredential::password) - 1; // 63

void copy_truncated(char *dst, size_t dst_size, const std::string &src) {
  const size_t copy_len = (src.size() < dst_size - 1) ? src.size() : dst_size - 1;
  std::memcpy(dst, src.data(), copy_len);
  dst[copy_len] = '\0';
}

} // namespace

void WifiCredentialStore::_slot_key(char *out, size_t out_size, const char *prefix, uint8_t index) {
  std::snprintf(out, out_size, "%s%u", prefix, static_cast<unsigned>(index));
}

uint8_t WifiCredentialStore::_load(WifiCredential *out) const {
  if (_store == nullptr) {
    return 0;
  }

  int raw_count = 0;
  if (_store->get_int(KEY_COUNT, raw_count) != ConfigStoreResult::OK) {
    // Missing count key => empty store. Not corruption, no self-heal.
    return 0;
  }

  // Clamp count into range; a clamp is corruption worth self-healing.
  int clamped = raw_count;
  if (clamped < 0) {
    clamped = 0;
  }
  if (clamped > WIFI_MAX_SAVED_NETWORKS) {
    clamped = WIFI_MAX_SAVED_NETWORKS;
  }
  bool dirty = (clamped != raw_count);

  uint8_t valid = 0;
  for (int i = 0; i < clamped; ++i) {
    char ssid_key[16];
    char pw_key[16];
    _slot_key(ssid_key, sizeof(ssid_key), KEY_SSID_PREFIX, static_cast<uint8_t>(i));
    _slot_key(pw_key, sizeof(pw_key), KEY_PW_PREFIX, static_cast<uint8_t>(i));

    std::string ssid;
    std::string password;
    // Missing SSID/password key, or empty SSID => skip the slot.
    if (_store->get_string(ssid_key, ssid) != ConfigStoreResult::OK || ssid.empty()) {
      dirty = true;
      continue;
    }
    if (_store->get_string(pw_key, password) != ConfigStoreResult::OK) {
      dirty = true;
      continue;
    }
    if (ssid.size() > SSID_MAX_LEN || password.size() > PW_MAX_LEN) {
      dirty = true; // truncated defensively below
    }

    copy_truncated(out[valid].ssid, sizeof(out[valid].ssid), ssid);
    copy_truncated(out[valid].password, sizeof(out[valid].password), password);
    ++valid;
  }

  if (dirty) {
    _save(out, valid);
  }
  return valid;
}

bool WifiCredentialStore::_save(const WifiCredential *list, uint8_t count) const {
  if (_store == nullptr) {
    return false;
  }

  bool ok = (_store->set_int(KEY_COUNT, count) == ConfigStoreResult::OK);
  for (uint8_t i = 0; i < WIFI_MAX_SAVED_NETWORKS; ++i) {
    char ssid_key[16];
    char pw_key[16];
    _slot_key(ssid_key, sizeof(ssid_key), KEY_SSID_PREFIX, i);
    _slot_key(pw_key, sizeof(pw_key), KEY_PW_PREFIX, i);
    if (i < count) {
      ok = (_store->set_string(ssid_key, list[i].ssid) == ConfigStoreResult::OK) && ok;
      ok = (_store->set_string(pw_key, list[i].password) == ConfigStoreResult::OK) && ok;
    } else {
      // Erase leftover slot keys so a stale tail never resurfaces;
      // not-found is fine, only a real backend error counts.
      const ConfigStoreResult sr = _store->erase(ssid_key);
      const ConfigStoreResult pr = _store->erase(pw_key);
      ok = (sr != ConfigStoreResult::ERROR) && (pr != ConfigStoreResult::ERROR) && ok;
    }
  }

  ok = (_store->commit() == ConfigStoreResult::OK) && ok;
  return ok;
}

WifiStatus WifiCredentialStore::add(const char *ssid, const char *password) {
  if (_store == nullptr) {
    return WifiStatus::Failed;
  }
  if (ssid == nullptr || ssid[0] == '\0' || std::strlen(ssid) > SSID_MAX_LEN) {
    return WifiStatus::InvalidArgument;
  }
  // Null password => open network (treated as ""). Empty is allowed.
  if (password != nullptr && std::strlen(password) > PW_MAX_LEN) {
    return WifiStatus::InvalidArgument;
  }

  WifiCredential list[WIFI_MAX_SAVED_NETWORKS];
  const uint8_t loaded = _load(list);

  // New entry first, then prior entries minus the duplicate SSID, capped.
  WifiCredential next[WIFI_MAX_SAVED_NETWORKS];
  std::strncpy(next[0].ssid, ssid, sizeof(next[0].ssid) - 1);
  if (password != nullptr) {
    std::strncpy(next[0].password, password, sizeof(next[0].password) - 1);
  }
  uint8_t count = 1;
  for (uint8_t i = 0; i < loaded && count < WIFI_MAX_SAVED_NETWORKS; ++i) {
    if (std::strcmp(list[i].ssid, next[0].ssid) == 0) {
      continue; // drop existing entry with the same SSID
    }
    next[count] = list[i];
    ++count;
  }

  return _save(next, count) ? WifiStatus::Ok : WifiStatus::Failed;
}

WifiStatus WifiCredentialStore::remove(const char *ssid) {
  if (_store == nullptr) {
    return WifiStatus::Failed;
  }
  if (ssid == nullptr || ssid[0] == '\0') {
    return WifiStatus::InvalidArgument;
  }

  WifiCredential list[WIFI_MAX_SAVED_NETWORKS];
  const uint8_t loaded = _load(list);

  WifiCredential next[WIFI_MAX_SAVED_NETWORKS];
  uint8_t count = 0;
  bool found = false;
  for (uint8_t i = 0; i < loaded; ++i) {
    if (!found && std::strcmp(list[i].ssid, ssid) == 0) {
      found = true; // drop the match, compact the rest
      continue;
    }
    next[count] = list[i];
    ++count;
  }

  if (!found) {
    return WifiStatus::NotFound;
  }
  return _save(next, count) ? WifiStatus::Ok : WifiStatus::Failed;
}

WifiStatus WifiCredentialStore::clear() {
  if (_store == nullptr) {
    return WifiStatus::Failed;
  }
  return _save(nullptr, 0) ? WifiStatus::Ok : WifiStatus::Failed;
}

uint8_t WifiCredentialStore::list(char (*out)[33], uint8_t max) const {
  WifiCredential loaded[WIFI_MAX_SAVED_NETWORKS];
  const uint8_t count = _load(loaded);
  const uint8_t limit = (count < max) ? count : max;
  for (uint8_t i = 0; i < limit; ++i) {
    std::strncpy(out[i], loaded[i].ssid, sizeof(out[i]) - 1);
    out[i][sizeof(out[i]) - 1] = '\0';
  }
  return count;
}

bool WifiCredentialStore::has_networks() const {
  WifiCredential loaded[WIFI_MAX_SAVED_NETWORKS];
  return _load(loaded) > 0;
}

uint8_t WifiCredentialStore::load_all(WifiCredential *out, uint8_t max) const {
  WifiCredential loaded[WIFI_MAX_SAVED_NETWORKS];
  const uint8_t count = _load(loaded);
  const uint8_t limit = (count < max) ? count : max;
  for (uint8_t i = 0; i < limit; ++i) {
    out[i] = loaded[i];
  }
  return count;
}
