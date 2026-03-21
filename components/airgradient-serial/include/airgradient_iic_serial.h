/**
 * AirGradient
 * https://airgradient.com
 *
 * I2C-to-UART Bridge Implementation using WK2132 chip
 * Embeds WK2132 driver logic without external dependencies
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AIRGRADIENT_IICSERIAL_HPP
#define AIRGRADIENT_IICSERIAL_HPP

#include <memory>

#include "driver/gpio.h"
#include "driver/i2c_master.h"

#include "wk2132_driver.h"

#include "airgradient_serial.h"

class WK2132Driver;

/**
 * I2C-to-UART bridge implementation using WK2132 chip.
 * Provides dual-channel UART over I2C interface.
 *
 * Features:
 * - Dual UART channels (channel 1 and 2)
 * - 256-byte hardware FIFO per channel
 * - 32-byte internal RX software buffer
 * - Configurable baud rates (2400-921600)
 * - Hardware reset support via GPIO pin
 * - Retry logic for initialization
 */
class AirgradientIICSerial : public AirgradientSerial {
public:
  static constexpr uint8_t SUB_UART_CHANNEL_1 = WK2132_SUBUART_CHANNEL_1;
  static constexpr uint8_t SUB_UART_CHANNEL_2 = WK2132_SUBUART_CHANNEL_2;

  /**
   * Constructor for I2C-UART bridge.
   *
   * @param i2c_bus_handle ESP-IDF I2C bus handle
   * @param subUartChannel Sub UART channel (WK2132_SUBUART_CHANNEL_1 or WK2132_SUBUART_CHANNEL_2)
   * @param IA1 DIP switch IA1 setting (0 or 1)
   * @param IA0 DIP switch IA0 setting (0 or 1)
   * @param iicResetIO GPIO pin number for WK2132 reset (-1 for no reset pin)
   */
  AirgradientIICSerial(i2c_master_bus_handle_t i2c_bus_handle,
                       uint8_t sub_uart_channel = SUB_UART_CHANNEL_1, uint8_t IA1 = 1,
                       uint8_t IA0 = 1, int iicResetIO = -1);
  ~AirgradientIICSerial();

  /**
   * Initialize I2C-UART bridge with baud rate.
   * Reset pin is configured via constructor.
   *
   * @param baud Baud rate (2400-921600)
   * @return true if successful, false otherwise
   */
  bool begin(int baud) override;

  /**
   * Deinitialize I2C-UART bridge.
   */
  void end() override;

  /**
   * Get number of bytes available to read.
   * Includes both hardware FIFO (256B) and software buffer (32B).
   *
   * @return Number of bytes available
   */
  int available() override;

  /**
   * Print null-terminated string.
   *
   * @param str String to print
   */
  void print(const char *str) override;

  /**
   * Write binary data.
   *
   * @param data Pointer to data buffer
   * @param len Length of data in bytes
   * @return Number of bytes written
   */
  int write(const uint8_t *data, int len) override;

  /**
   * Read single byte.
   * Data is lazily loaded from hardware FIFO to software buffer.
   *
   * @return Byte value (0-255), or -1 if no data available
   */
  int read() override;

private:
  static constexpr size_t RX_BUFFER_SIZE = 32;

  // Initialization constants
  static constexpr int MAX_RETRY_INIT = 3;
  static constexpr int RETRY_DELAY_MS = 500;

  gpio_num_t _iicResetIO; // Reset GPIO pin
  std::unique_ptr<WK2132Driver> _wk2132;

  // RX software buffer (circular buffer)
  volatile uint8_t _rx_buffer_head;
  volatile uint8_t _rx_buffer_tail;
  unsigned char _rx_buffer[RX_BUFFER_SIZE];
};

#endif // AIRGRADIENT_IICSERIAL_HPP
