/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_WIFI_CREDENTIAL_STORE_H
#define AG_WIFI_CREDENTIAL_STORE_H

#include "../types/wifi_types.h"

class ConfigStore;

/// A single saved Wi-Fi credential (SSID + password). Buffers are sized
/// to the public limits (SSID 32 bytes + NUL, password 63 bytes + NUL).
struct WifiCredential {
  char ssid[33] = {};
  char password[64] = {};
};

/// Pure-C++ credential store over an injected ConfigStore: up to
/// WIFI_MAX_SAVED_NETWORKS SSID/password pairs, newest-first, oldest
/// evicted on overflow. The whole list is rewritten per mutation (writes
/// are rare); loads self-heal partial/corrupt state.
///
/// ISR-safe: no
/// Thread-safe: no
/// Blocking: yes (mutations perform a bounded NVS commit)
class WifiCredentialStore {
public:
  explicit WifiCredentialStore(ConfigStore &store) : _store(store) {}

  /// Insert {ssid, password} newest; a same-SSID entry is dropped first
  /// (re-add refreshes password + marks newest), tail capped to the max.
  /// InvalidArgument: null/empty/overlong SSID or overlong password.
  /// Failed: commit failed.
  WifiStatus add(const char *ssid, const char *password);

  /// Remove the entry matching ssid (case-sensitive exact).
  /// InvalidArgument: null/empty SSID. NotFound: no match. Failed: commit
  /// failed.
  WifiStatus remove(const char *ssid);

  /// Erase all saved entries. Failed: commit failed.
  WifiStatus clear();

  /// Copy up to max SSIDs (newest-first) into out. Returns the number of
  /// saved entries.
  uint8_t list(char (*out)[33], uint8_t max) const;

  /// True when at least one network is saved.
  bool has_networks() const;

  /// Load the full list (SSID + password, newest-first) into out. Returns
  /// the entry count.
  uint8_t load_all(WifiCredential *out, uint8_t max) const;

private:
  // Load + normalise into out (capacity WIFI_MAX_SAVED_NETWORKS). Rewrites
  // the store when anything was skipped / clamped / truncated (self-heal).
  uint8_t _load(WifiCredential *out) const;

  // Rewrite count + slots, erase any leftover slot keys, commit.
  bool _save(const WifiCredential *list, uint8_t count) const;

  static void _slot_key(char *out, size_t out_size, const char *prefix, uint8_t index);

  ConfigStore &_store;
};

#endif // AG_WIFI_CREDENTIAL_STORE_H
