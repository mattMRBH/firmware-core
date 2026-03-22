/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "rtc_payload_cache_storage.h"

#include <cstring>

#if !defined(TEST_HOST) && defined(__has_include)
#if __has_include("esp_attr.h")
#include "esp_attr.h"
#endif
#endif

#ifndef RTC_DATA_ATTR
#define RTC_DATA_ATTR
#endif

namespace {

RTC_DATA_ATTR PayloadCacheStorageData s_payload_cache_storage = {};

} // namespace

bool RtcPayloadCacheStorage::load(PayloadCacheStorageData &data) {
  memcpy(&data, &s_payload_cache_storage, sizeof(data));
  return true;
}

bool RtcPayloadCacheStorage::save(const PayloadCacheStorageData &data) {
  memcpy(&s_payload_cache_storage, &data, sizeof(data));
  return true;
}

bool RtcPayloadCacheStorage::clear() {
  s_payload_cache_storage = PayloadCacheStorageData{};
  return true;
}
