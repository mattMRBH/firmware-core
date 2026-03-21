/**
 * AirGradient
 * https://airgradient.com
 *
 * WK2132 vendor driver wrapper
 * Based on DFRobot WK2132 library internals
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef WK2132_DRIVER_H
#define WK2132_DRIVER_H

#include <stddef.h>
#include <stdint.h>

#include "driver/i2c_master.h"

#include "wk2132_registers.h"

class WK2132Driver {
public:
  WK2132Driver(i2c_master_bus_handle_t i2c_bus_handle, uint8_t sub_uart_channel, uint8_t ia1,
               uint8_t ia0);

  int begin(unsigned long baud, uint8_t format = WK2132_8N1,
            WK2132_CommunicationMode mode = WK2132_MODE_NORMAL,
            WK2132_LineBreakOutput opt = WK2132_LINEBREAK_NORMAL);
  void reset();

  bool read_rx_fifo_count(uint8_t *count);
  WK2132_FsrReg_t read_fifo_state_reg();
  bool write_fifo(const uint8_t *buf, size_t size);
  bool read_fifo_byte(uint8_t *value);

private:
  struct DeviceInfo {
    uint8_t address;
    i2c_master_dev_handle_t dev_handle;
  };

  i2c_master_bus_handle_t _i2c_bus_handle;
  uint8_t _addr;
  uint8_t _sub_serial_channel;
  DeviceInfo _dev_handle_map[WK2132_MAX_ADDR];

  int wk2132Begin(unsigned long baud, uint8_t format, WK2132_CommunicationMode mode,
                  WK2132_LineBreakOutput opt);
  void wk2132SubSerialConfig(uint8_t sub_uart_channel);
  void wk2132SubSerialGlobalRegEnable(uint8_t sub_uart_channel, WK2132_GlobalRegType type);
  void wk2132SetSubSerialBaudRate(unsigned long baud);
  void wk2132SetSubSerialConfigReg(uint8_t format, WK2132_CommunicationMode mode,
                                   WK2132_LineBreakOutput opt);
  void wk2132SubSerialPageSwitch(WK2132_PageNumber page);
  void wk2132SubSerialRegConfig(uint8_t reg, void *value);
  uint8_t wk2132GetGlobalRegType(WK2132_GlobalRegType type);
  uint8_t wk2132SubSerialChnnlSwitch(uint8_t sub_uart_channel);
  uint8_t wk2132UpdateAddr(uint8_t pre, uint8_t sub_uart_channel, uint8_t obj);
  void wk2132WriteReg(uint8_t reg, const void *buf, size_t size);
  uint8_t wk2132ReadReg(uint8_t reg, void *buf, size_t size);
  i2c_master_dev_handle_t wk2132GetDeviceHandle(uint8_t addr);
};

#endif // WK2132_DRIVER_H
