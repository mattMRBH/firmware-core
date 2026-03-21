/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef PAYLOAD_CACHE_H
#define PAYLOAD_CACHE_H

#include <cstdint>

#include "../hal/payload_cache_storage.h"

class PayloadCache {
public:
  PayloadCache(PayloadCacheStorage &storage, uint16_t max_size);

  void restore();
  void backup() const;
  uint16_t get_size() const;

  void clean();
  void push(const PayloadCacheType &payload);
  bool pop(PayloadCacheType &payload);
  bool peek_at_index(uint16_t index, PayloadCacheType &payload) const;

private:
  void _reset();
  bool _is_empty() const;

  PayloadCacheStorage &_storage;
  uint16_t _max_cache_size;
  uint16_t _head;
  uint16_t _tail;
  PayloadCacheType _payloads[PAYLOAD_CACHE_MAX_SIZE];
};

#endif // PAYLOAD_CACHE_H
