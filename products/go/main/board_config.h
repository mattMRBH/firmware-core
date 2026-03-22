/**
 * AirGradient Go — Board Configuration
 *
 * Pin assignments and peripheral constants for the AGo board.
 * All GPIO values are TBD placeholders (-1) until the hardware schematic
 * is finalized.  I2C device addresses use datasheet defaults.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BOARD_CONFIG_H
#define BOARD_CONFIG_H

#include <cstdint>

#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <driver/uart.h>

// ---------------------------------------------------------------------------
// I2C bus
// ---------------------------------------------------------------------------

inline constexpr int PIN_I2C_SDA = -1; // TBD from schematic
inline constexpr int PIN_I2C_SCL = -1; // TBD from schematic

// ---------------------------------------------------------------------------
// SPI — Display (SSD1680 e-paper)
// ---------------------------------------------------------------------------

inline constexpr spi_host_device_t SPI_HOST_DISPLAY = SPI2_HOST;
inline constexpr int PIN_DISPLAY_MOSI = -1; // TBD
inline constexpr int PIN_DISPLAY_CLK = -1;  // TBD
inline constexpr int PIN_DISPLAY_CS = -1;   // TBD
inline constexpr int PIN_DISPLAY_DC = -1;   // TBD
inline constexpr int PIN_DISPLAY_RST = -1;  // TBD
inline constexpr int PIN_DISPLAY_BUSY = -1; // TBD

// ---------------------------------------------------------------------------
// SPI — NAND flash
// May share the SPI host with display or use a separate host depending on
// hardware routing.
// ---------------------------------------------------------------------------

inline constexpr spi_host_device_t SPI_HOST_NAND = SPI3_HOST;
inline constexpr int PIN_NAND_MOSI = -1; // TBD
inline constexpr int PIN_NAND_MISO = -1; // TBD
inline constexpr int PIN_NAND_CLK = -1;  // TBD
inline constexpr int PIN_NAND_CS = -1;   // TBD

// ---------------------------------------------------------------------------
// UART — GPS (NmeaGps)
// ---------------------------------------------------------------------------

inline constexpr uart_port_t UART_PORT_GPS = UART_NUM_1;
inline constexpr int PIN_GPS_RX = -1; // TBD
inline constexpr int PIN_GPS_TX = -1; // TBD
inline constexpr int GPS_BAUD = 9600;

// ---------------------------------------------------------------------------
// UART — CO2 sensor (S8 or Sunlight)
// ---------------------------------------------------------------------------

inline constexpr uart_port_t UART_PORT_CO2 = UART_NUM_0;
inline constexpr int PIN_CO2_RX = -1; // TBD
inline constexpr int PIN_CO2_TX = -1; // TBD
inline constexpr int CO2_BAUD = 9600;

// ---------------------------------------------------------------------------
// UART — PM sensor (PMS5003)
// Serial interface may be native UART or I2C-to-UART bridge depending on
// hardware routing.  Using UART_NUM_2 as a placeholder.
// ---------------------------------------------------------------------------

inline constexpr uart_port_t UART_PORT_PM = UART_NUM_2;
inline constexpr int PIN_PM_RX = -1; // TBD
inline constexpr int PIN_PM_TX = -1; // TBD
inline constexpr int PM_BAUD = 9600;

// ---------------------------------------------------------------------------
// I2C device addresses
// ---------------------------------------------------------------------------

inline constexpr uint8_t I2C_ADDR_SHT40 = 0x44;
inline constexpr uint8_t I2C_ADDR_SGP41 = 0x59;
inline constexpr uint8_t I2C_ADDR_BMS = 0x6B; // BQ25672 / BQ25798 default
inline constexpr uint8_t I2C_ADDR_CAP1203 = 0x28;

// ---------------------------------------------------------------------------
// Physical buttons
// ---------------------------------------------------------------------------

inline constexpr int PIN_BUTTON_POWER = -1; // TBD
inline constexpr int PIN_BUTTON_BOOT = -1;  // TBD

// ---------------------------------------------------------------------------
// Capacitive touch interrupt
// ---------------------------------------------------------------------------

inline constexpr int PIN_CAP_INT = -1;      // TBD
inline constexpr int TOUCH_DELTA_SENSE = 0; // max sensitivity

// ---------------------------------------------------------------------------
// Power enables (populated from schematic)
// Uncomment and set real GPIO numbers once the schematic is finalized.
// ---------------------------------------------------------------------------

// inline constexpr int PIN_PMS_ENABLE = -1;   // -1 if always on
// inline constexpr int PIN_GPS_ENABLE = -1;   // -1 if always on

#endif // BOARD_CONFIG_H
