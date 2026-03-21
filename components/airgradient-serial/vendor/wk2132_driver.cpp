/**
 * AirGradient
 * https://airgradient.com
 *
 * WK2132 vendor driver wrapper
 * Based on DFRobot WK2132 library internals
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "wk2132_driver.h"

#include <cstring>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static constexpr const char *TAG = "WK2132";

WK2132Driver::WK2132Driver(i2c_master_bus_handle_t i2c_bus_handle, uint8_t sub_uart_channel,
                           uint8_t ia1, uint8_t ia0)
    : _i2c_bus_handle(i2c_bus_handle), _sub_serial_channel(sub_uart_channel) {
  _addr = (ia1 << 6) | (ia0 << 5) | WK2132_IIC_ADDR_FIXED;
  memset(_dev_handle_map, 0, sizeof(_dev_handle_map));
}

int WK2132Driver::begin(unsigned long baud, uint8_t format, WK2132_CommunicationMode mode,
                        WK2132_LineBreakOutput opt) {
  return wk2132Begin(baud, format, mode, opt);
}

void WK2132Driver::reset() {
  wk2132SubSerialGlobalRegEnable(_sub_serial_channel, WK2132_GLOBAL_RESET);
}

bool WK2132Driver::read_rx_fifo_count(uint8_t *count) {
  if (count == nullptr) {
    return false;
  }
  return wk2132ReadReg(WK2132_REG_RFCNT, count, 1) == 1;
}

WK2132_FsrReg_t WK2132Driver::read_fifo_state_reg() {
  WK2132_FsrReg_t fsr = {};
  wk2132ReadReg(WK2132_REG_FSR, &fsr, sizeof(fsr));
  return fsr;
}

bool WK2132Driver::write_fifo(const uint8_t *buf, size_t size) {
  if (buf == nullptr) {
    return false;
  }

  _addr = wk2132UpdateAddr(_addr, _sub_serial_channel, WK2132_OBJECT_FIFO);
  i2c_master_dev_handle_t dev_handle = wk2132GetDeviceHandle(_addr);
  if (dev_handle == nullptr) {
    return false;
  }

  size_t left = size;
  const uint8_t *cursor = buf;
  while (left > 0) {
    size_t chunk = (left > WK2132_IIC_BUFFER_SIZE) ? WK2132_IIC_BUFFER_SIZE : left;

    esp_err_t err = i2c_master_transmit(dev_handle, cursor, chunk, 500);
    if (err != ESP_OK) {
      ESP_LOGV(TAG, "FIFO write failed to 0x%02x", _addr);
      return false;
    }

    vTaskDelay(pdMS_TO_TICKS(10));
    cursor += chunk;
    left -= chunk;
  }

  return true;
}

bool WK2132Driver::read_fifo_byte(uint8_t *value) {
  if (value == nullptr) {
    return false;
  }
  return wk2132ReadReg(WK2132_REG_FDAT, value, 1) == 1;
}

int WK2132Driver::wk2132Begin(unsigned long baud, uint8_t format, WK2132_CommunicationMode mode,
                              WK2132_LineBreakOutput opt) {
  uint8_t val = 0;
  uint8_t channel = wk2132SubSerialChnnlSwitch(WK2132_SUBUART_CHANNEL_1);
  if (wk2132ReadReg(WK2132_REG_GENA, &val, 1) != 1) {
    ESP_LOGV(TAG, "Failed to read GENA register");
    wk2132SubSerialChnnlSwitch(channel);
    return WK2132_ERR_READ;
  }

#ifndef ARDUINO_ARCH_NRF5
  if ((val & 0x80) == 0) {
    ESP_LOGV(TAG, "GENA register validation failed");
    wk2132SubSerialChnnlSwitch(channel);
    return WK2132_ERR_REGDATA;
  }
#endif

  wk2132SubSerialChnnlSwitch(channel);
  wk2132SubSerialConfig(_sub_serial_channel);
  wk2132SetSubSerialBaudRate(baud);
  wk2132SetSubSerialConfigReg(format, mode, opt);

  return WK2132_ERR_OK;
}

void WK2132Driver::wk2132SubSerialConfig(uint8_t sub_uart_channel) {
  ESP_LOGV(TAG, "Configuring sub UART channel %d", sub_uart_channel);

  ESP_LOGV(TAG, "Enable sub UART clock");
  wk2132SubSerialGlobalRegEnable(sub_uart_channel, WK2132_GLOBAL_CLOCK);

  ESP_LOGV(TAG, "Software reset sub UART");
  wk2132SubSerialGlobalRegEnable(sub_uart_channel, WK2132_GLOBAL_RESET);

  ESP_LOGV(TAG, "Enable global interrupt");
  wk2132SubSerialGlobalRegEnable(sub_uart_channel, WK2132_GLOBAL_INTERRUPT);

  ESP_LOGV(TAG, "Switch to page 0");
  wk2132SubSerialPageSwitch(WK2132_PAGE0);

  ESP_LOGV(TAG, "Configure interrupt enable");
  WK2132_SierReg_t sier = {
      .rFTrig = 0x01, .rxOvt = 0x01, .tfTrig = 0x01, .tFEmpty = 0x01, .rsv = 0x00, .fErr = 0x01};
  wk2132SubSerialRegConfig(WK2132_REG_SIER, &sier);

  ESP_LOGV(TAG, "Configure FIFO");
  WK2132_FcrReg_t fcr = {
      .rfRst = 0x01,
      .tfRst = 0x00,
      .rfEn = 0x01,
      .tfEn = 0x01,
      .rfTrig = 0x00,
      .tfTrig = 0x00,
  };
  wk2132SubSerialRegConfig(WK2132_REG_FCR, &fcr);

  ESP_LOGV(TAG, "Enable RX/TX");
  WK2132_ScrReg_t scr = {.rxEn = 0x01, .txEn = 0x01, .sleepEn = 0x01, .rsv = 0x00};
  wk2132SubSerialRegConfig(WK2132_REG_SCR, &scr);
}

void WK2132Driver::wk2132SubSerialGlobalRegEnable(uint8_t sub_uart_channel,
                                                  WK2132_GlobalRegType type) {
  if (sub_uart_channel > WK2132_SUBUART_CHANNEL_ALL) {
    ESP_LOGV(TAG, "Invalid sub UART channel");
    return;
  }

  uint8_t val = 0;
  uint8_t reg_addr = wk2132GetGlobalRegType(type);
  uint8_t channel = wk2132SubSerialChnnlSwitch(WK2132_SUBUART_CHANNEL_1);

  ESP_LOGV(TAG, "Global reg enable: reg=0x%02x", reg_addr);

  if (wk2132ReadReg(reg_addr, &val, 1) != 1) {
    ESP_LOGV(TAG, "Failed to read global register");
    wk2132SubSerialChnnlSwitch(channel);
    return;
  }

  ESP_LOGV(TAG, "Before: 0x%02x", val);

  switch (sub_uart_channel) {
  case WK2132_SUBUART_CHANNEL_1:
    val |= 0x01;
    break;
  case WK2132_SUBUART_CHANNEL_2:
    val |= 0x02;
    break;
  default:
    val |= 0x03;
    break;
  }

  wk2132WriteReg(reg_addr, &val, 1);
  wk2132ReadReg(reg_addr, &val, 1);
  ESP_LOGV(TAG, "After: 0x%02x", val);

  wk2132SubSerialChnnlSwitch(channel);
}

void WK2132Driver::wk2132SetSubSerialBaudRate(unsigned long baud) {
  uint8_t scr = 0x00;
  uint8_t clear = 0x00;
  wk2132ReadReg(WK2132_REG_SCR, &scr, 1);
  wk2132SubSerialRegConfig(WK2132_REG_SCR, &clear);

  uint16_t val_integer = WK2132_FOSC / (baud * 16) - 1;
  uint16_t val_decimal = (WK2132_FOSC % (baud * 16)) / (baud * 16);

  uint8_t baud1 = static_cast<uint8_t>(val_integer >> 8);
  uint8_t baud0 = static_cast<uint8_t>(val_integer & 0x00FF);

  while (val_decimal > 0x0A) {
    val_decimal /= 0x0A;
  }
  uint8_t baud_pres = static_cast<uint8_t>(val_decimal);

  wk2132SubSerialPageSwitch(WK2132_PAGE1);
  wk2132SubSerialRegConfig(WK2132_REG_BAUD1, &baud1);
  wk2132SubSerialRegConfig(WK2132_REG_BAUD0, &baud0);
  wk2132SubSerialRegConfig(WK2132_REG_PRES, &baud_pres);

  ESP_LOGV(TAG, "Baud config: baud1=0x%02x, baud0=0x%02x, pres=0x%02x", baud1, baud0, baud_pres);

  wk2132SubSerialPageSwitch(WK2132_PAGE0);
  wk2132SubSerialRegConfig(WK2132_REG_SCR, &scr);
}

void WK2132Driver::wk2132SetSubSerialConfigReg(uint8_t format, WK2132_CommunicationMode mode,
                                               WK2132_LineBreakOutput opt) {
  uint8_t val = 0;
  _addr = wk2132UpdateAddr(_addr, _sub_serial_channel, WK2132_OBJECT_REGISTER);

  if (wk2132ReadReg(WK2132_REG_LCR, &val, 1) != 1) {
    ESP_LOGV(TAG, "Failed to read LCR");
    return;
  }

  ESP_LOGV(TAG, "LCR before: 0x%02x", val);

  WK2132_LcrReg_t lcr = *reinterpret_cast<WK2132_LcrReg_t *>(&val);
  lcr.format = format;
  lcr.irEn = mode;
  lcr.lBreak = opt;
  val = *reinterpret_cast<uint8_t *>(&lcr);

  wk2132WriteReg(WK2132_REG_LCR, &val, 1);
  wk2132ReadReg(WK2132_REG_LCR, &val, 1);
  ESP_LOGV(TAG, "LCR after: 0x%02x", val);
}

void WK2132Driver::wk2132SubSerialPageSwitch(WK2132_PageNumber page) {
  if (page >= WK2132_PAGE_TOTAL) {
    return;
  }

  uint8_t val = 0;
  if (wk2132ReadReg(WK2132_REG_SPAGE, &val, 1) != 1) {
    ESP_LOGV(TAG, "Failed to read SPAGE");
    return;
  }

  switch (page) {
  case WK2132_PAGE0:
    val &= 0xFE;
    break;
  case WK2132_PAGE1:
    val |= 0x01;
    break;
  default:
    break;
  }

  ESP_LOGV(TAG, "SPAGE before: 0x%02x", val);
  wk2132WriteReg(WK2132_REG_SPAGE, &val, 1);
  wk2132ReadReg(WK2132_REG_SPAGE, &val, 1);
  ESP_LOGV(TAG, "SPAGE after: 0x%02x", val);
}

void WK2132Driver::wk2132SubSerialRegConfig(uint8_t reg, void *value) {
  uint8_t val = 0;
  wk2132ReadReg(reg, &val, 1);
  ESP_LOGV(TAG, "Reg 0x%02x before: 0x%02x", reg, val);

  val |= *static_cast<uint8_t *>(value);
  wk2132WriteReg(reg, &val, 1);

  wk2132ReadReg(reg, &val, 1);
  ESP_LOGV(TAG, "Reg 0x%02x after: 0x%02x", reg, val);
}

uint8_t WK2132Driver::wk2132GetGlobalRegType(WK2132_GlobalRegType type) {
  switch (type) {
  case WK2132_GLOBAL_CLOCK:
    return WK2132_REG_GENA;
  case WK2132_GLOBAL_RESET:
    return WK2132_REG_GRST;
  case WK2132_GLOBAL_INTERRUPT:
    return WK2132_REG_GIER;
  default:
    ESP_LOGV(TAG, "Invalid global register type");
    return 0;
  }
}

uint8_t WK2132Driver::wk2132SubSerialChnnlSwitch(uint8_t sub_uart_channel) {
  uint8_t previous_channel = _sub_serial_channel;
  _sub_serial_channel = sub_uart_channel;
  return previous_channel;
}

uint8_t WK2132Driver::wk2132UpdateAddr(uint8_t pre, uint8_t sub_uart_channel, uint8_t obj) {
  WK2132_IICAddr_t addr = {
      .type = obj,
      .uart = sub_uart_channel,
      .addrPre = static_cast<uint8_t>(static_cast<int>(pre) >> 3),
  };
  return *reinterpret_cast<uint8_t *>(&addr);
}

void WK2132Driver::wk2132WriteReg(uint8_t reg, const void *buf, size_t size) {
  if (buf == nullptr) {
    ESP_LOGV(TAG, "Write register: null pointer");
    return;
  }

  _addr = wk2132UpdateAddr(_addr, _sub_serial_channel, WK2132_OBJECT_REGISTER);
  i2c_master_dev_handle_t dev_handle = wk2132GetDeviceHandle(_addr);
  if (dev_handle == nullptr) {
    ESP_LOGV(TAG, "Failed to get device handle");
    return;
  }

  const uint8_t *data = static_cast<const uint8_t *>(buf);
  size_t tx_size = size + 1;
  uint8_t *tx_buf = new uint8_t[tx_size];
  tx_buf[0] = reg;
  memcpy(&tx_buf[1], data, size);

  esp_err_t err = i2c_master_transmit(dev_handle, tx_buf, tx_size, 500);
  if (err != ESP_OK) {
    ESP_LOGV(TAG, "Register write failed");
  }

  delete[] tx_buf;
}

uint8_t WK2132Driver::wk2132ReadReg(uint8_t reg, void *buf, size_t size) {
  if (buf == nullptr) {
    ESP_LOGV(TAG, "Read register: null pointer");
    return 0;
  }

  _addr = wk2132UpdateAddr(_addr, _sub_serial_channel, WK2132_OBJECT_REGISTER);
  _addr &= 0xFE;
  i2c_master_dev_handle_t dev_handle = wk2132GetDeviceHandle(_addr);
  if (dev_handle == nullptr) {
    ESP_LOGV(TAG, "Failed to get device handle");
    return 0;
  }

  esp_err_t err = i2c_master_transmit(dev_handle, &reg, 1, 500);
  if (err != ESP_OK) {
    ESP_LOGV(TAG, "Failed to send register address 0x%02x", reg);
    return 0;
  }

  err = i2c_master_receive(dev_handle, static_cast<uint8_t *>(buf), size, 500);
  if (err != ESP_OK) {
    ESP_LOGV(TAG, "Failed to read from device 0x%02x", _addr);
    return 0;
  }

  return static_cast<uint8_t>(size);
}

i2c_master_dev_handle_t WK2132Driver::wk2132GetDeviceHandle(uint8_t addr) {
  for (int i = 0; i < WK2132_MAX_ADDR; i++) {
    if (_dev_handle_map[i].address == addr) {
      return _dev_handle_map[i].dev_handle;
    }
  }

  for (int i = 0; i < WK2132_MAX_ADDR; i++) {
    if (_dev_handle_map[i].address == 0) {
      i2c_device_config_t dev_cfg = {
          .dev_addr_length = I2C_ADDR_BIT_LEN_7,
          .device_address = addr,
          .scl_speed_hz = 100000,
          .scl_wait_us = 0,
          .flags = {},
      };

      _dev_handle_map[i].address = addr;

      esp_err_t ret =
          i2c_master_bus_add_device(_i2c_bus_handle, &dev_cfg, &_dev_handle_map[i].dev_handle);
      if (ret != ESP_OK) {
        ESP_LOGV(TAG, "Failed to add I2C device at address 0x%02x", addr);
        _dev_handle_map[i].address = 0;
        return nullptr;
      }

      ESP_LOGV(TAG, "Created I2C device handle for address 0x%02x", addr);
      return _dev_handle_map[i].dev_handle;
    }
  }

  ESP_LOGV(TAG, "No space for new I2C device at address 0x%02x", addr);
  return nullptr;
}
