/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_WIFI_FAKE_CONFIG_STORE_H
#define AG_WIFI_FAKE_CONFIG_STORE_H

#include <map>
#include <string>

#include "config_store.h"

/// In-memory ConfigStore for host tests. Mirrors the typed key-value
/// semantics of the NVS backend: get returns NOT_FOUND for absent keys,
/// erase removes a key, commit is a no-op success.
class FakeConfigStore : public ConfigStore {
public:
  ConfigStoreResult get_int(const char *key, int &out) override {
    auto it = _ints.find(key);
    if (it == _ints.end()) {
      return ConfigStoreResult::NOT_FOUND;
    }
    out = it->second;
    return ConfigStoreResult::OK;
  }
  ConfigStoreResult set_int(const char *key, int value) override {
    _ints[key] = value;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult get_bool(const char *key, bool &out) override {
    auto it = _bools.find(key);
    if (it == _bools.end()) {
      return ConfigStoreResult::NOT_FOUND;
    }
    out = it->second;
    return ConfigStoreResult::OK;
  }
  ConfigStoreResult set_bool(const char *key, bool value) override {
    _bools[key] = value;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult get_string(const char *key, std::string &out) override {
    auto it = _strings.find(key);
    if (it == _strings.end()) {
      return ConfigStoreResult::NOT_FOUND;
    }
    out = it->second;
    return ConfigStoreResult::OK;
  }
  ConfigStoreResult set_string(const char *key, const std::string &value) override {
    _strings[key] = value;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult get_float(const char *key, float &out) override {
    auto it = _floats.find(key);
    if (it == _floats.end()) {
      return ConfigStoreResult::NOT_FOUND;
    }
    out = it->second;
    return ConfigStoreResult::OK;
  }
  ConfigStoreResult set_float(const char *key, float value) override {
    _floats[key] = value;
    return ConfigStoreResult::OK;
  }

  ConfigStoreResult erase(const char *key) override {
    bool erased = false;
    erased |= _ints.erase(key) > 0;
    erased |= _bools.erase(key) > 0;
    erased |= _strings.erase(key) > 0;
    erased |= _floats.erase(key) > 0;
    return erased ? ConfigStoreResult::OK : ConfigStoreResult::NOT_FOUND;
  }
  ConfigStoreResult commit() override {
    commit_count += 1;
    return commit_should_fail ? ConfigStoreResult::ERROR : ConfigStoreResult::OK;
  }

  // -- Test inspection / fault injection helpers --
  std::map<std::string, int> _ints;
  std::map<std::string, std::string> _strings;
  std::map<std::string, bool> _bools;
  std::map<std::string, float> _floats;
  int commit_count = 0;
  bool commit_should_fail = false; // force commit() to fail (persist-error tests)
};

#endif // AG_WIFI_FAKE_CONFIG_STORE_H
