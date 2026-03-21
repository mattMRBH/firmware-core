/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef RTC_PAYLOAD_CACHE_STORAGE_H
#define RTC_PAYLOAD_CACHE_STORAGE_H

#include "../hal/payload_cache_storage.h"

class RtcPayloadCacheStorage : public PayloadCacheStorage {
public:
  bool load(PayloadCacheStorageData &data) override;
  bool save(const PayloadCacheStorageData &data) override;
  bool clear() override;
};

#endif // RTC_PAYLOAD_CACHE_STORAGE_H
