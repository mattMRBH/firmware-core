/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_SCAN_FILTER_H
#define AG_PROVISIONING_SCAN_FILTER_H

#include <cstddef>
#include <cstdint>

#include "types/wifi_types.h"

// Shared pure-C++ utility used by both provisioning transports.
//
// Reduces a raw scan result array (from WifiManager::on_scan_complete) to
// a filtered, deduplicated, RSSI-sorted view suitable for display:
//
//   1. drop entries with empty SSIDs
//   2. drop entries weaker than SCAN_MIN_RSSI_DBM (-75 dBm)
//   3. deduplicate by SSID, keeping the strongest RSSI per SSID
//   4. sort by RSSI descending
//   5. cap at SCAN_MAX_NETWORKS entries
//
// Operates in place on the caller's output buffer to avoid heap traffic.
namespace ScanFilter {

inline constexpr int8_t SCAN_MIN_RSSI_DBM = -75;
inline constexpr size_t SCAN_MAX_NETWORKS = 30;

// Apply the filter pipeline. `in` may alias `out`. The function reads up
// to `in_count` entries from `in`, writes filtered entries into `out`,
// and returns the number written (0..SCAN_MAX_NETWORKS).
size_t apply(const WifiScanEntry *in, uint16_t in_count, WifiScanEntry *out, size_t out_capacity);

} // namespace ScanFilter

#endif // AG_PROVISIONING_SCAN_FILTER_H
