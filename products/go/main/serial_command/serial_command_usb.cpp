#include "serial_command/serial_command.h"

#include <fcntl.h>
#include <unistd.h>

#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_err.h"
#include "freertos/FreeRTOS.h"

bool UsbSerialCommandChannel::initialize() {
  if (_initialized) {
    return true;
  }

  if (!usb_serial_jtag_is_driver_installed()) {
    usb_serial_jtag_driver_config_t config = {
        .tx_buffer_size = SERIAL_COMMAND_USB_TX_BUFFER_BYTES,
        .rx_buffer_size = SERIAL_COMMAND_USB_RX_BUFFER_BYTES,
    };
    if (usb_serial_jtag_driver_install(&config) != ESP_OK) {
      return false;
    }
  }

  usb_serial_jtag_vfs_use_driver();
  _tx_fd = open("/dev/secondary", O_WRONLY);
  if (_tx_fd < 0) {
    return false;
  }

  _initialized = true;
  return true;
}

int UsbSerialCommandChannel::read_bytes(char *buffer, size_t buffer_size, uint32_t timeout_ms) {
  if (!_initialized || buffer == nullptr || buffer_size == 0) {
    return -1;
  }
  return usb_serial_jtag_read_bytes(buffer, static_cast<uint32_t>(buffer_size),
                                    pdMS_TO_TICKS(timeout_ms));
}

bool UsbSerialCommandChannel::write_response(const char *response, size_t response_size) {
  if (!_initialized || response == nullptr || response_size == 0) {
    return false;
  }
  return write(_tx_fd, response, response_size) == static_cast<ssize_t>(response_size);
}
