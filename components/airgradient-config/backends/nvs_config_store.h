/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef NVS_CONFIG_STORE_H
#define NVS_CONFIG_STORE_H

#include <string>

#include "esp_err.h"
#include "nvs.h"

#include "../hal/config_store.h"

class NvsConfigStore : public ConfigStore {
public:
  explicit NvsConfigStore(const char *namespace_name,
                          nvs_open_mode_t open_mode = NVS_READWRITE);
  ~NvsConfigStore() override;

  ConfigStoreResult get_int(const char *key, int &out) override;
  ConfigStoreResult set_int(const char *key, int value) override;

  ConfigStoreResult get_bool(const char *key, bool &out) override;
  ConfigStoreResult set_bool(const char *key, bool value) override;

  ConfigStoreResult get_string(const char *key, std::string &out) override;
  ConfigStoreResult set_string(const char *key,
                               const std::string &value) override;

  ConfigStoreResult erase(const char *key) override;
  ConfigStoreResult commit() override;

private:
  bool _ensure_open();
  ConfigStoreResult _map_error(esp_err_t err) const;

  std::string _namespace_name;
  nvs_open_mode_t _open_mode;
  nvs_handle_t _handle = 0;
  bool _is_open = false;
};

#endif // NVS_CONFIG_STORE_H
