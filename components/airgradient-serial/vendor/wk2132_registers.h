/**
 * AirGradient
 * https://airgradient.com
 *
 * WK2132 I2C-to-Dual-UART Bridge Register Definitions
 * Based on DFRobot WK2132 library
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef WK2132_REGISTERS_HPP
#define WK2132_REGISTERS_HPP

#include <stdint.h>

// WK2132 Constants
#define WK2132_FOSC 14745600L           // External crystal frequency 14.7456MHz
#define WK2132_IIC_BUFFER_SIZE 32       // I2C buffer size (32 bytes max per transaction)
#define WK2132_SERIAL_RX_BUFFER_SIZE 32 // Internal RX buffer size
#define WK2132_MAX_ADDR 8               // Maximum number of device addresses

// Sub UART Channels
#define WK2132_SUBUART_CHANNEL_1 0x00
#define WK2132_SUBUART_CHANNEL_2 0x01
#define WK2132_SUBUART_CHANNEL_ALL 0x11

// IIC Address Configuration
#define WK2132_IIC_ADDR_FIXED 0x10 // Bits 4:3 are fixed at 1:0

// Object Types
#define WK2132_OBJECT_REGISTER 0x00 // Register access
#define WK2132_OBJECT_FIFO 0x01     // FIFO buffer access

// Data Formats
#define WK2132_8N1 0x00 // 8 data bits, No parity, 1 stop bit
#define WK2132_8N2 0x01 // 8 data bits, No parity, 2 stop bits
#define WK2132_8Z1 0x08 // 8 data bits, Zero parity, 1 stop bit
#define WK2132_8Z2 0x09 // 8 data bits, Zero parity, 2 stop bits
#define WK2132_8O1 0x0A // 8 data bits, Odd parity, 1 stop bit
#define WK2132_8O2 0x0B // 8 data bits, Odd parity, 2 stop bits
#define WK2132_8E1 0x0C // 8 data bits, Even parity, 1 stop bit
#define WK2132_8E2 0x0D // 8 data bits, Even parity, 2 stop bits
#define WK2132_8F1 0x0E // 8 data bits, Force 1 parity, 1 stop bit
#define WK2132_8F2 0x0F // 8 data bits, Force 1 parity, 2 stop bits

// Error Codes
#define WK2132_ERR_OK 0
#define WK2132_ERR_REGDATA -1
#define WK2132_ERR_READ -2

// Global Registers
#define WK2132_REG_GENA 0x00 // Global control register (clock enable)
#define WK2132_REG_GRST 0x01 // Global reset register
#define WK2132_REG_GMUT 0x02 // Global main UART control register
#define WK2132_REG_GIER 0x10 // Global interrupt enable register
#define WK2132_REG_GIFR 0x11 // Global interrupt flag register

// Page Control Register
#define WK2132_REG_SPAGE 0x03 // Sub UART page control register

// Sub UART Registers - Page 0
#define WK2132_REG_SCR 0x04   // Sub UART control register
#define WK2132_REG_LCR 0x05   // Sub UART line configuration register
#define WK2132_REG_FCR 0x06   // Sub UART FIFO control register
#define WK2132_REG_SIER 0x07  // Sub UART interrupt enable register
#define WK2132_REG_SIFR 0x08  // Sub UART interrupt flag register
#define WK2132_REG_TFCNT 0x09 // Sub UART transmit FIFO count register
#define WK2132_REG_RFCNT 0x0A // Sub UART receive FIFO count register
#define WK2132_REG_FSR 0x0B   // Sub UART FIFO status register
#define WK2132_REG_LSR 0x0C   // Sub UART line status register
#define WK2132_REG_FDAT 0x0D  // Sub UART FIFO data register

// Sub UART Registers - Page 1
#define WK2132_REG_BAUD1 0x04 // Sub UART baud rate high byte
#define WK2132_REG_BAUD0 0x05 // Sub UART baud rate low byte
#define WK2132_REG_PRES 0x06  // Sub UART baud rate prescaler
#define WK2132_REG_RFTL 0x07  // Sub UART RX FIFO trigger level
#define WK2132_REG_TFTL 0x08  // Sub UART TX FIFO trigger level

/**
 * IIC Address Structure
 * Format: [0][IA1][IA0][1][0][C1][C0][OBJ]
 * - Bit 7: Always 0
 * - Bits 6-5: IA1, IA0 (DIP switch configurable)
 * - Bits 4-3: Fixed as 1, 0
 * - Bits 2-1: C1, C0 (channel: 00=ch1, 01=ch2)
 * - Bit 0: Object type (0=register, 1=FIFO)
 */
typedef struct {
  uint8_t type : 1;    // 0=register, 1=FIFO
  uint8_t uart : 2;    // Sub UART channel (00=ch1, 01=ch2)
  uint8_t addrPre : 5; // Address prefix (includes IA1, IA0, fixed bits)
} __attribute__((packed)) WK2132_IICAddr_t;

/**
 * SIER - Sub UART Interrupt Enable Register
 */
typedef struct {
  uint8_t rFTrig : 1;  // RX FIFO trigger interrupt enable
  uint8_t rxOvt : 1;   // RX FIFO overflow timeout interrupt enable
  uint8_t tfTrig : 1;  // TX FIFO trigger interrupt enable
  uint8_t tFEmpty : 1; // TX FIFO empty interrupt enable
  uint8_t rsv : 3;     // Reserved
  uint8_t fErr : 1;    // FIFO error interrupt enable
} __attribute__((packed)) WK2132_SierReg_t;

/**
 * FCR - FIFO Control Register
 */
typedef struct {
  uint8_t rfRst : 1;  // RX FIFO reset (auto-clear)
  uint8_t tfRst : 1;  // TX FIFO reset (auto-clear)
  uint8_t rfEn : 1;   // RX FIFO enable
  uint8_t tfEn : 1;   // TX FIFO enable
  uint8_t rfTrig : 2; // RX FIFO trigger level (00=8B, 01=16B, 10=24B, 11=28B)
  uint8_t tfTrig : 2; // TX FIFO trigger level (00=8B, 01=16B, 10=24B, 11=28B)
} __attribute__((packed)) WK2132_FcrReg_t;

/**
 * SCR - Sub UART Control Register
 */
typedef struct {
  uint8_t rxEn : 1;    // RX enable
  uint8_t txEn : 1;    // TX enable
  uint8_t sleepEn : 1; // Sleep mode enable
  uint8_t rsv : 5;     // Reserved
} __attribute__((packed)) WK2132_ScrReg_t;

/**
 * LCR - Line Configuration Register
 */
typedef struct {
  uint8_t format : 4; // Data format (parity, stop bits)
  uint8_t irEn : 1;   // IR mode enable
  uint8_t lBreak : 1; // Line break output control
  uint8_t rsv : 2;    // Reserved
} __attribute__((packed)) WK2132_LcrReg_t;

/**
 * FSR - FIFO Status Register
 */
typedef struct {
  uint8_t tBusy : 1; // TX busy flag
  uint8_t tFull : 1; // TX FIFO full flag
  uint8_t tDat : 1;  // TX FIFO has data flag
  uint8_t rDat : 1;  // RX FIFO has data flag
  uint8_t rFpe : 1;  // RX parity error flag
  uint8_t rFfe : 1;  // RX frame error flag
  uint8_t rFbi : 1;  // RX line break error flag
  uint8_t rFoe : 1;  // RX overflow error flag
} __attribute__((packed)) WK2132_FsrReg_t;

// Page Numbers
enum WK2132_PageNumber { WK2132_PAGE0 = 0, WK2132_PAGE1 = 1, WK2132_PAGE_TOTAL = 2 };

// Global Register Types
enum WK2132_GlobalRegType {
  WK2132_GLOBAL_CLOCK = 0,    // Clock control
  WK2132_GLOBAL_RESET = 1,    // Reset control
  WK2132_GLOBAL_INTERRUPT = 2 // Interrupt control
};

// Communication Modes
enum WK2132_CommunicationMode {
  WK2132_MODE_NORMAL = 0
  // WK2132_MODE_IRDA = 1  // Not implemented
};

// Line Break Output
enum WK2132_LineBreakOutput {
  WK2132_LINEBREAK_NORMAL = 0
  // WK2132_LINEBREAK_FORCE = 1  // Not implemented
};

#if (WK2132_SERIAL_RX_BUFFER_SIZE > 256)
typedef uint16_t wk2132_rx_buffer_index_t;
#else
typedef uint8_t wk2132_rx_buffer_index_t;
#endif

#endif // WK2132_REGISTERS_HPP
