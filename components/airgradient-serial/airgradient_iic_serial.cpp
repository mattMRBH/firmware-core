/**
 * AirGradient
 * https://airgradient.com
 *
 * I2C-to-UART Bridge Implementation using WK2132 chip
 * Embeds WK2132 driver logic from DFRobot library
 *
 * Based on:
 * DFRobot_IICSerial library (MIT License)
 * Copyright (c) 2010 DFRobot Co.Ltd (http://www.dfrobot.com)
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "airgradient_iic_serial.h"

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include <cstring>

static constexpr const char *TAG = "AGIICSerial";

AirgradientIICSerial::AirgradientIICSerial(i2c_master_bus_handle_t i2c_bus_handle,
                                           uint8_t sub_uart_channel, uint8_t IA1, uint8_t IA0,
                                           int iicResetIO)
    : _iicResetIO(GPIO_NUM_NC),
      _wk2132(std::make_unique<WK2132Driver>(i2c_bus_handle, sub_uart_channel, IA1, IA0)),
      _rx_buffer_head(0), _rx_buffer_tail(0) {

  // Store reset pin
  if (iicResetIO != -1) {
    _iicResetIO = static_cast<gpio_num_t>(iicResetIO);
  } else {
    _iicResetIO = GPIO_NUM_NC;
  }

  // Clear RX buffer
  memset((void *)_rx_buffer, 0, sizeof(_rx_buffer));
}

AirgradientIICSerial::~AirgradientIICSerial() = default;

bool AirgradientIICSerial::begin(int baud) {
  if (isInitialized) {
    ESP_LOGW(TAG, "IICSerial already initialized");
    return true;
  }

  // Configure reset GPIO if provided
  if (_iicResetIO != GPIO_NUM_NC) {
    gpio_reset_pin(_iicResetIO);
    gpio_set_direction(_iicResetIO, GPIO_MODE_OUTPUT);
    gpio_set_level(_iicResetIO, 1); // Release reset
  }

  // Retry initialization
  int counter = 0;
  bool opened = false;
  do {
    if (_wk2132->begin(baud) == WK2132_ERR_OK) {
      opened = true;
      break;
    }

    ESP_LOGW(TAG, "IICSerial failed to open serial line, retry...");
    counter++;
    vTaskDelay(pdMS_TO_TICKS(RETRY_DELAY_MS));
  } while (counter < MAX_RETRY_INIT);

  if (!opened) {
    ESP_LOGE(TAG, "IICSerial failed to open serial line, giving up");
    return false;
  }

  ESP_LOGI(TAG, "IICSerial successfully opened serial line");
  isInitialized = true;
  return true;
}

void AirgradientIICSerial::end() {
  _wk2132->reset();
  isInitialized = false;
}

int AirgradientIICSerial::available() {
  // Read FIFO count from hardware
  uint8_t val = 0;
  if (!_wk2132->read_rx_fifo_count(&val)) {
    ESP_LOGV(TAG, "Failed to read FIFO count");
    return 0;
  }

  int fifo_count = (int)val;

  // If FIFO count is 0 but FIFO status shows data available, count is 256
  if (fifo_count == 0) {
    WK2132_FsrReg_t fsr = _wk2132->read_fifo_state_reg();
    if (fsr.rDat == 1) {
      fifo_count = 256;
    }
  }

  // Add software buffer count
  int buffer_count =
      (unsigned int)(RX_BUFFER_SIZE + _rx_buffer_head - _rx_buffer_tail) % RX_BUFFER_SIZE;

  return fifo_count + buffer_count;
}

void AirgradientIICSerial::print(const char *str) {
  if (isDebug) {
    // Prevent carriage return to stdout
    if (strcmp(str, "\r\n") == 0) {
      printf("\n");
    } else {
      printf("%s", str);
    }
  }

  if (str != nullptr) {
    write((const uint8_t *)str, strlen(str));
  }
}

int AirgradientIICSerial::write(const uint8_t *data, int len) {
  if (data == nullptr || len <= 0) {
    return 0;
  }

  // Check if TX FIFO is full
  WK2132_FsrReg_t fsr = _wk2132->read_fifo_state_reg();
  if (fsr.tFull == 1) {
    ESP_LOGV(TAG, "TX FIFO full");
    return 0;
  }

  // Write to FIFO
  if (!_wk2132->write_fifo(data, len)) {
    return 0;
  }
  return len;
}

int AirgradientIICSerial::read() {
  // Lazy-load data from hardware FIFO to software buffer
  int fifo_bytes =
      available() -
      ((unsigned int)(RX_BUFFER_SIZE + _rx_buffer_head - _rx_buffer_tail) % RX_BUFFER_SIZE);

  for (int i = 0; i < fifo_bytes; i++) {
    uint8_t next_head = (_rx_buffer_head + 1) % RX_BUFFER_SIZE;

    if (next_head != _rx_buffer_tail) {
      // Read one byte from hardware FIFO
      uint8_t val = 0;
      if (_wk2132->read_fifo_byte(&val)) {
        _rx_buffer[_rx_buffer_head] = val;
        _rx_buffer_head = next_head;
      }
    } else {
      // Software buffer full
      break;
    }
  }

  // Return data from software buffer
  if (_rx_buffer_head == _rx_buffer_tail) {
    return -1;
  }

  unsigned char c = _rx_buffer[_rx_buffer_tail];
  _rx_buffer_tail = (_rx_buffer_tail + 1) % RX_BUFFER_SIZE;

  if (isDebug) {
    if (c != '\r') {
      printf("%c", c);
    }
  }

  return c;
}
