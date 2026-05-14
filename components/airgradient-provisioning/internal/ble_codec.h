/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_BLE_CODEC_H
#define AG_PROVISIONING_BLE_CODEC_H

#include <cstddef>
#include <cstdint>

#include "../types/provisioning_types.h"
#include "types/wifi_types.h"

// Pure C++ JSON codec for the BLE provisioning transport.
//
// All functions operate on byte buffers and are fully host-testable.
// JSON encoding/decoding uses cJSON internally.
namespace BleCodec {

// Number of networks per BLE scan notification page (spec constant).
inline constexpr size_t NETWORKS_PER_PAGE = 3;

// Parse a BLE credential JSON payload into ProvisioningData.
//
// Expected format:
//   {"ssid":"...","password":"...","disableCloud":true}
//
// Only "ssid" is required. "password" defaults to empty (open network).
// "disableCloud" defaults to false when absent (backward compatible
// with old mobile apps).
//
// Returns true on success.
bool parse_credentials(const uint8_t *data, size_t len, ProvisioningData &out);

// Encode one page of BLE scan results as JSON.
//
// Output format (one notification per page):
//   {"wifi":[{"s":"SSID","r":-45,"o":0},...],"page":1,"tpage":4,"found":10}
//
// @param entries        Networks for this page only
// @param entries_count  Number of entries in this page (≤ NETWORKS_PER_PAGE)
// @param page           Current page number (1-based)
// @param total_pages    Total number of pages
// @param total_found    Total number of filtered networks across all pages
// @param buf            Output buffer
// @param buf_size       Output buffer size
// @return               Bytes written to buf, 0 on failure
size_t encode_scan_page(const WifiScanEntry *entries, size_t entries_count, size_t page,
                        size_t total_pages, size_t total_found, uint8_t *buf, size_t buf_size);

// Encode an empty scan result: {"found":0}
//
// @return Bytes written to buf, 0 on failure
size_t encode_scan_empty(uint8_t *buf, size_t buf_size);

// Encode a BLE status notification: {"status":<code>}
//
// @return Bytes written to buf, 0 on failure
size_t encode_status(uint8_t status_code, uint8_t *buf, size_t buf_size);

} // namespace BleCodec

#endif // AG_PROVISIONING_BLE_CODEC_H
