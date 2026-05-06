/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "nvs_config_store.h"

NvsConfigStore::NvsConfigStore(const char *namespace_name, nvs_open_mode_t open_mode)
    : _namespace_name(namespace_name ? namespace_name : ""), _open_mode(open_mode) {}

NvsConfigStore::~NvsConfigStore() {
  if (_is_open) {
    nvs_close(_handle);
  }
}

ConfigStoreResult NvsConfigStore::get_int(const char *key, int &out) {
  if (!_ensure_open()) {
    return ConfigStoreResult::ERROR;
  }

  int32_t value = 0;
  const esp_err_t err = nvs_get_i32(_handle, key, &value);
  if (err == ESP_OK) {
    out = static_cast<int>(value);
  }

  return _map_error(err);
}

ConfigStoreResult NvsConfigStore::set_int(const char *key, int value) {
  if (!_ensure_open()) {
    return ConfigStoreResult::ERROR;
  }

  return _map_error(nvs_set_i32(_handle, key, static_cast<int32_t>(value)));
}

ConfigStoreResult NvsConfigStore::get_bool(const char *key, bool &out) {
  if (!_ensure_open()) {
    return ConfigStoreResult::ERROR;
  }

  uint8_t value = 0;
  const esp_err_t err = nvs_get_u8(_handle, key, &value);
  if (err == ESP_OK) {
    out = (value != 0);
  }

  return _map_error(err);
}

ConfigStoreResult NvsConfigStore::set_bool(const char *key, bool value) {
  if (!_ensure_open()) {
    return ConfigStoreResult::ERROR;
  }

  return _map_error(nvs_set_u8(_handle, key, value ? 1U : 0U));
}

ConfigStoreResult NvsConfigStore::get_string(const char *key, std::string &out) {
  if (!_ensure_open()) {
    return ConfigStoreResult::ERROR;
  }

  size_t required_size = 0;
  esp_err_t err = nvs_get_str(_handle, key, nullptr, &required_size);
  if (err != ESP_OK) {
    return _map_error(err);
  }

  std::string value(required_size, '\0');
  err = nvs_get_str(_handle, key, value.data(), &required_size);
  if (err != ESP_OK) {
    return _map_error(err);
  }

  if (!value.empty() && value.back() == '\0') {
    value.pop_back();
  }

  out = value;
  return ConfigStoreResult::OK;
}

ConfigStoreResult NvsConfigStore::set_string(const char *key, const std::string &value) {
  if (!_ensure_open()) {
    return ConfigStoreResult::ERROR;
  }

  return _map_error(nvs_set_str(_handle, key, value.c_str()));
}

ConfigStoreResult NvsConfigStore::erase(const char *key) {
  if (!_ensure_open()) {
    return ConfigStoreResult::ERROR;
  }

  return _map_error(nvs_erase_key(_handle, key));
}

ConfigStoreResult NvsConfigStore::commit() {
  if (!_ensure_open()) {
    return ConfigStoreResult::ERROR;
  }

  return _map_error(nvs_commit(_handle));
}

bool NvsConfigStore::_ensure_open() {
  if (_is_open) {
    return true;
  }

  if (_namespace_name.empty()) {
    return false;
  }

  const esp_err_t err = nvs_open(_namespace_name.c_str(), _open_mode, &_handle);
  if (err != ESP_OK) {
    return false;
  }

  _is_open = true;
  return true;
}

ConfigStoreResult NvsConfigStore::_map_error(esp_err_t err) const {
  if (err == ESP_OK) {
    return ConfigStoreResult::OK;
  }

  if (err == ESP_ERR_NVS_NOT_FOUND) {
    return ConfigStoreResult::NOT_FOUND;
  }

  return ConfigStoreResult::ERROR;
}
