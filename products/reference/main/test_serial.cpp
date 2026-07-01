#include "test_serial.h"

#include "esp_log.h"

#include "airgradient_iic_serial.h"
#include "airgradient_uart.h"
#include "board_config.h"

static constexpr const char *TAG = "test_serial";

void run_test_serial(i2c_master_bus_handle_t i2c_bus) {
  ESP_LOGI(TAG, "--- Serial test start ---");

  // AirgradientIICSerial — IIC-to-UART bridge, channel 1
  AirgradientIICSerial iic_serial(i2c_bus, AirgradientIICSerial::SUB_UART_CHANNEL_1, 0, 1,
                                  PIN_IIC_RESET);
  if (!iic_serial.begin(9600)) {
    ESP_LOGE(TAG, "IICSerial CH1 begin failed");
  } else {
    ESP_LOGI(TAG, "IICSerial CH1: OK");
  }

  // AirgradientIICSerial — IIC-to-UART bridge, channel 2
  AirgradientIICSerial iic_serial2(i2c_bus, AirgradientIICSerial::SUB_UART_CHANNEL_2, 0, 1,
                                   PIN_IIC_RESET);
  if (!iic_serial2.begin(9600)) {
    ESP_LOGE(TAG, "IICSerial CH2 begin failed");
  } else {
    ESP_LOGI(TAG, "IICSerial CH2: OK");
  }

  // AirgradientUART — native UART for CO2 sensor
  AirgradientUART uart(UART_PORT_CO2, PIN_UART_RX_CO2, PIN_UART_TX_CO2);
  if (!uart.begin(UART_BAUD_CO2)) {
    ESP_LOGE(TAG, "UART begin failed");
  } else {
    ESP_LOGI(TAG, "UART (port %d, %d baud): OK", static_cast<int>(UART_PORT_CO2), UART_BAUD_CO2);
  }

  ESP_LOGI(TAG, "--- Serial test done ---");
}
