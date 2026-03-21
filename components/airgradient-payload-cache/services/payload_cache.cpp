/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "payload_cache.h"

#include <cstring>

PayloadCache::PayloadCache(PayloadCacheStorage &storage, uint16_t max_size)
    : _storage(storage), _max_cache_size(max_size), _head(0), _tail(0),
      _payloads{} {
  if (_max_cache_size < 2) {
    _max_cache_size = 2;
  }

  if (_max_cache_size > PAYLOAD_CACHE_MAX_SIZE) {
    _max_cache_size = PAYLOAD_CACHE_MAX_SIZE;
  }

  _reset();
}

void PayloadCache::restore() {
  PayloadCacheStorageData data = {};
  if (!_storage.load(data)) {
    _reset();
    return;
  }

  if (data.head >= _max_cache_size || data.tail >= _max_cache_size) {
    _reset();
    return;
  }

  _head = data.head;
  _tail = data.tail;
  memcpy(_payloads, data.payloads, sizeof(_payloads));
}

void PayloadCache::backup() const {
  PayloadCacheStorageData data = {};
  data.head = _head;
  data.tail = _tail;
  memcpy(data.payloads, _payloads, sizeof(_payloads));
  _storage.save(data);
}

uint16_t PayloadCache::get_size() const {
  if (_tail >= _head) {
    return _tail - _head;
  }

  return _max_cache_size - _head + _tail;
}

void PayloadCache::clean() {
  _reset();
  _storage.clear();
}

void PayloadCache::push(const PayloadCacheType &payload) {
  _payloads[_tail] = payload;
  _tail = (_tail + 1) % _max_cache_size;

  if (_tail == _head) {
    _head = (_head + 1) % _max_cache_size;
  }

  backup();
}

bool PayloadCache::pop(PayloadCacheType &payload) {
  if (_is_empty()) {
    return false;
  }

  payload = _payloads[_head];
  _head = (_head + 1) % _max_cache_size;
  backup();
  return true;
}

bool PayloadCache::peek_at_index(uint16_t index,
                                 PayloadCacheType &payload) const {
  if (index >= get_size()) {
    return false;
  }

  const uint16_t actual_index = (_head + index) % _max_cache_size;
  payload = _payloads[actual_index];
  return true;
}

void PayloadCache::_reset() {
  _head = 0;
  _tail = 0;
}

bool PayloadCache::_is_empty() const { return _head == _tail; }
