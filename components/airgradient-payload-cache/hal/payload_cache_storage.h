/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef PAYLOAD_CACHE_STORAGE_H
#define PAYLOAD_CACHE_STORAGE_H

#include "payload_cache_types.h"

class PayloadCacheStorage {
public:
  virtual ~PayloadCacheStorage() = default;

  virtual bool load(PayloadCacheStorageData &data) = 0;
  virtual bool save(const PayloadCacheStorageData &data) = 0;
  virtual bool clear() = 0;
};

#endif // PAYLOAD_CACHE_STORAGE_H
