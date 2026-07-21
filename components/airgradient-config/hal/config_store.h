/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef CONFIG_STORE_H
#define CONFIG_STORE_H

#include <string>

enum class ConfigStoreResult {
  OK,
  NOT_FOUND,
  ERROR,
};

class ConfigStore {
public:
  virtual ~ConfigStore() = default;

  virtual ConfigStoreResult get_int(const char *key, int &out) = 0;
  virtual ConfigStoreResult set_int(const char *key, int value) = 0;

  virtual ConfigStoreResult get_bool(const char *key, bool &out) = 0;
  virtual ConfigStoreResult set_bool(const char *key, bool value) = 0;

  virtual ConfigStoreResult get_string(const char *key, std::string &out) = 0;
  virtual ConfigStoreResult set_string(const char *key, const std::string &value) = 0;

  virtual ConfigStoreResult get_float(const char *key, float &out) = 0;
  virtual ConfigStoreResult set_float(const char *key, float value) = 0;

  virtual ConfigStoreResult erase(const char *key) = 0;
  virtual ConfigStoreResult commit() = 0;
};

#endif // CONFIG_STORE_H
