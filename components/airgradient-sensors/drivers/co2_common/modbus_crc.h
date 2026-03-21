/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef MODBUS_CRC_HPP
#define MODBUS_CRC_HPP

#include <stdint.h>

/**
 * @brief Calculate Modbus RTU CRC16
 *
 * CRC routine extracted from Modbus specification:
 * https://modbus.org/docs/Modbus_over_serial_line_V1_02.pdf
 *
 * @param buf Message buffer to calculate CRC upon
 * @param len Length of message in bytes
 * @return uint16_t CRC16 value (little-endian format)
 */
uint16_t modbus_crc16(uint8_t *buf, uint16_t len);

#endif // MODBUS_CRC_HPP
