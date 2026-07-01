#ifndef TEST_PAYLOAD_CACHE_H
#define TEST_PAYLOAD_CACHE_H

// Exercises PayloadCache with RtcPayloadCacheStorage: restore from RTC,
// push test payloads, verify get_size, pop and compare values, then clean.
void run_test_payload_cache();

#endif // TEST_PAYLOAD_CACHE_H
