/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BLE_TYPES_H
#define BLE_TYPES_H

#include <cstddef>
#include <cstdint>
#include <functional>

// BLE characteristic property flags. Combine with | to set multiple properties.
namespace BleProperty {
inline constexpr uint8_t READ = 0x01;
inline constexpr uint8_t WRITE = 0x02;
inline constexpr uint8_t WRITE_NR = 0x04; // write with no response
inline constexpr uint8_t NOTIFY = 0x08;
inline constexpr uint8_t INDICATE = 0x10;
} // namespace BleProperty

// Invoked when a client connects. conn_handle identifies the connection.
using BleConnectCallback = std::function<void(uint16_t conn_handle)>;

// Invoked when a client disconnects. reason is a BLE stack error code.
using BleDisconnectCallback = std::function<void(uint16_t conn_handle, int reason)>;

// Invoked when a client writes to a characteristic.
// data and len describe the written payload. The buffer is only valid for the
// duration of the callback; callers must copy if they need to retain the data.
using BleWriteCallback = std::function<void(const uint8_t *data, size_t len)>;

#endif // BLE_TYPES_H
