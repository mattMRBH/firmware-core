#include "go_display.h"

#ifndef TEST_HOST

#include <cmath>
#include <cstdio>
#include <cstring>

#include <driver/gpio.h>
#include <esp_heap_caps.h>
#include <esp_log.h>

#include "rtos.h"

extern "C" {
#include "u8x8.h"
}

static constexpr const char *TAG = "go_display";

// ===========================================================================
// Anonymous namespace: display driver + rendering helpers
// ===========================================================================

namespace {

// ---------------------------------------------------------------------------
// Display driver — SSD1680 SPI protocol (file-local)
// ---------------------------------------------------------------------------

constexpr int WIDTH_PX = 128;
constexpr int HEIGHT_PX = 250;
constexpr int FRAME_BYTES = (WIDTH_PX * HEIGHT_PX) / 8; // 4000
constexpr size_t TX_BOUNCE_BYTES = 4096;

// Error-propagation macro for driver functions returning esp_err_t.
// Intentionally unscoped so it works in non-void-returning functions.
#define DISP_RETURN_ON_ERR(expr)                                                                   \
  do {                                                                                             \
    const esp_err_t _e = (expr);                                                                   \
    if (_e != ESP_OK)                                                                              \
      return _e;                                                                                   \
  } while (0)

struct DriverState {
  bool inited = false;
  bool bus_acquired = false;

  spi_host_device_t spi_host = SPI2_HOST;
  int spi_clock_hz = 4000000;

  gpio_num_t pin_cs = GPIO_NUM_NC;
  gpio_num_t pin_dc = GPIO_NUM_NC;
  gpio_num_t pin_rst = GPIO_NUM_NC;
  gpio_num_t pin_busy = GPIO_NUM_NC;

  spi_device_handle_t spi = nullptr;
  uint8_t *tx_bounce = nullptr;
};

DriverState g_driver;

// --- Low-level GPIO helpers ---

inline void set_cs(int level) { gpio_set_level(g_driver.pin_cs, level); }
inline void set_dc(int level) { gpio_set_level(g_driver.pin_dc, level); }
inline void set_rst(int level) { gpio_set_level(g_driver.pin_rst, level); }

void delay_ms(uint32_t ms) {
  if (ms == 0)
    return;
  RTOS::delay_ms(ms);
}

// BUSY=1 means controller processing; BUSY=0 means ready.
void wait_busy_low() {
  while (gpio_get_level(g_driver.pin_busy) != 0) {
    RTOS::delay_ms(1);
  }
}

// --- SPI transfer primitives ---

esp_err_t spi_write_byte(uint8_t value) {
  spi_transaction_t t = {};
  t.flags = SPI_TRANS_USE_TXDATA;
  t.length = 8;
  t.tx_data[0] = value;
  return spi_device_polling_transmit(g_driver.spi, &t);
}

esp_err_t spi_write_bulk(const uint8_t *data, size_t len) {
  size_t offset = 0;
  while (offset < len) {
    const size_t chunk = (len - offset > TX_BOUNCE_BYTES) ? TX_BOUNCE_BYTES : (len - offset);
    memcpy(g_driver.tx_bounce, data + offset, chunk);
    spi_transaction_t t = {};
    t.length = chunk * 8;
    t.tx_buffer = g_driver.tx_bounce;
    DISP_RETURN_ON_ERR(spi_device_polling_transmit(g_driver.spi, &t));
    offset += chunk;
  }
  return ESP_OK;
}

// --- SSD1680 command/data protocol ---

esp_err_t write_cmd(uint8_t command) {
  set_cs(0);
  set_dc(0);
  const esp_err_t err = spi_write_byte(command);
  set_cs(1);
  return err;
}

esp_err_t write_data(uint8_t data) {
  set_cs(0);
  set_dc(1);
  const esp_err_t err = spi_write_byte(data);
  set_cs(1);
  return err;
}

esp_err_t write_data_bytes(const uint8_t *data, size_t len) {
  set_cs(0);
  set_dc(1);
  const esp_err_t err = spi_write_bulk(data, len);
  set_cs(1);
  return err;
}

// --- Driver public API ---

esp_err_t driver_init(const DisplayService::Config &cfg) {
  if (g_driver.inited)
    return ESP_OK;

  g_driver.spi_host = cfg.spi_host;
  g_driver.spi_clock_hz = cfg.clock_hz;
  g_driver.pin_cs = static_cast<gpio_num_t>(cfg.pin_cs);
  g_driver.pin_dc = static_cast<gpio_num_t>(cfg.pin_dc);
  g_driver.pin_rst = static_cast<gpio_num_t>(cfg.pin_rst);
  g_driver.pin_busy = static_cast<gpio_num_t>(cfg.pin_busy);

  // Configure output GPIOs (CS, DC, RST)
  gpio_config_t out_cfg = {};
  out_cfg.pin_bit_mask = (1ULL << cfg.pin_cs) | (1ULL << cfg.pin_dc) | (1ULL << cfg.pin_rst);
  out_cfg.mode = GPIO_MODE_OUTPUT;
  out_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  out_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  out_cfg.intr_type = GPIO_INTR_DISABLE;
  DISP_RETURN_ON_ERR(gpio_config(&out_cfg));

  // Configure BUSY input
  gpio_config_t in_cfg = {};
  in_cfg.pin_bit_mask = 1ULL << cfg.pin_busy;
  in_cfg.mode = GPIO_MODE_INPUT;
  in_cfg.pull_up_en = GPIO_PULLUP_DISABLE;
  in_cfg.pull_down_en = GPIO_PULLDOWN_DISABLE;
  in_cfg.intr_type = GPIO_INTR_DISABLE;
  DISP_RETURN_ON_ERR(gpio_config(&in_cfg));

  set_cs(1);
  set_dc(1);
  set_rst(1);

  // Attach SPI device (CS managed manually, not by ESP-IDF driver)
  spi_device_interface_config_t devcfg = {};
  devcfg.mode = 0;
  devcfg.clock_speed_hz = cfg.clock_hz;
  devcfg.spics_io_num = -1;
  devcfg.queue_size = 1;
  devcfg.flags = SPI_DEVICE_NO_DUMMY;
  DISP_RETURN_ON_ERR(spi_bus_add_device(cfg.spi_host, &devcfg, &g_driver.spi));

  // DMA-capable bounce buffer for chunked SPI writes
  g_driver.tx_bounce = static_cast<uint8_t *>(heap_caps_malloc(TX_BOUNCE_BYTES, MALLOC_CAP_DMA));
  if (g_driver.tx_bounce == nullptr)
    return ESP_ERR_NO_MEM;

  delay_ms(100);
  g_driver.inited = true;
  ESP_LOGI(TAG, "EPD driver init done");
  return ESP_OK;
}

esp_err_t driver_bus_acquire() {
  if (!g_driver.inited || g_driver.spi == nullptr)
    return ESP_ERR_INVALID_STATE;
  if (g_driver.bus_acquired)
    return ESP_ERR_INVALID_STATE;

  // ESP-IDF spi_device_acquire_bus() only supports portMAX_DELAY (infinite
  // wait).  Finite timeouts return ESP_ERR_INVALID_ARG.
  DISP_RETURN_ON_ERR(spi_device_acquire_bus(g_driver.spi, portMAX_DELAY));
  g_driver.bus_acquired = true;
  return ESP_OK;
}

void driver_bus_release() {
  if (!g_driver.bus_acquired)
    return;
  if (g_driver.spi != nullptr) {
    spi_device_release_bus(g_driver.spi);
  }
  g_driver.bus_acquired = false;
}

// Full SSD1680 initialization sequence (see UI-IMPLEMENTATION.md §2.3.3).
esp_err_t driver_hw_init_full() {
  // Hardware reset
  set_rst(0);
  delay_ms(10);
  set_rst(1);
  delay_ms(10);
  wait_busy_low();

  // Software reset
  DISP_RETURN_ON_ERR(write_cmd(0x12));
  wait_busy_low();

  // Driver output control: gate lines = HEIGHT_PX - 1
  DISP_RETURN_ON_ERR(write_cmd(0x01));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Data entry mode: X increment, Y decrement
  DISP_RETURN_ON_ERR(write_cmd(0x11));
  DISP_RETURN_ON_ERR(write_data(0x01));

  // RAM-X window: 0 to 15 (0..127 pixels)
  DISP_RETURN_ON_ERR(write_cmd(0x44));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(WIDTH_PX / 8 - 1)));

  // RAM-Y window: 249 to 0
  DISP_RETURN_ON_ERR(write_cmd(0x45));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Border waveform (full mode)
  DISP_RETURN_ON_ERR(write_cmd(0x3C));
  DISP_RETURN_ON_ERR(write_data(0x05));

  // Display update control
  DISP_RETURN_ON_ERR(write_cmd(0x21));
  DISP_RETURN_ON_ERR(write_data(0x00));
  DISP_RETURN_ON_ERR(write_data(0x80));

  // Built-in temperature sensor
  DISP_RETURN_ON_ERR(write_cmd(0x18));
  DISP_RETURN_ON_ERR(write_data(0x80));

  // RAM-X counter to 0
  DISP_RETURN_ON_ERR(write_cmd(0x4E));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // RAM-Y counter to HEIGHT_PX - 1
  DISP_RETURN_ON_ERR(write_cmd(0x4F));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  wait_busy_low();

  return ESP_OK;
}

// Write frame to both SSD1680 RAM planes + trigger full update.
esp_err_t driver_set_basemap(const uint8_t *data) {
  DISP_RETURN_ON_ERR(write_cmd(0x24));
  DISP_RETURN_ON_ERR(write_data_bytes(data, FRAME_BYTES));
  DISP_RETURN_ON_ERR(write_cmd(0x26));
  DISP_RETURN_ON_ERR(write_data_bytes(data, FRAME_BYTES));

  // Trigger full update
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0xF7));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();
  return ESP_OK;
}

// Prepare controller for partial update mode.
esp_err_t driver_part_begin() {
  // Hardware reset
  set_rst(0);
  delay_ms(10);
  set_rst(1);
  delay_ms(10);
  wait_busy_low();

  // Re-establish Y mapping after reset
  DISP_RETURN_ON_ERR(write_cmd(0x01));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>((HEIGHT_PX - 1) / 256)));
  DISP_RETURN_ON_ERR(write_data(0x00));

  // Border waveform (partial mode — differs from full init)
  DISP_RETURN_ON_ERR(write_cmd(0x3C));
  DISP_RETURN_ON_ERR(write_data(0x80));

  // Data entry mode (same as full init)
  DISP_RETURN_ON_ERR(write_cmd(0x11));
  DISP_RETURN_ON_ERR(write_data(0x01));

  return ESP_OK;
}

// Write pixel data to a rectangular region for partial update.
// Coordinates are logical screen coordinates (Y=0 at top).
esp_err_t driver_part_write_region(unsigned int x_start_px, unsigned int y_start_px,
                                   const uint8_t *data, unsigned int height_px,
                                   unsigned int width_px) {
  const unsigned int x_start = x_start_px / 8;
  const unsigned int x_end = x_start + width_px / 8 - 1;

  // Map logical top-origin Y to controller Y (inverted).
  // Controller Y=249 corresponds to screen top (Y=0).
  const unsigned int y_start = (HEIGHT_PX - 1) - y_start_px;
  const unsigned int y_end = (HEIGHT_PX - 1) - (y_start_px + height_px - 1);

  // Set RAM window
  DISP_RETURN_ON_ERR(write_cmd(0x44));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(x_start)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(x_end)));

  DISP_RETURN_ON_ERR(write_cmd(0x45));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start / 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_end % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_end / 256)));

  // Set RAM cursor
  DISP_RETURN_ON_ERR(write_cmd(0x4E));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(x_start)));
  DISP_RETURN_ON_ERR(write_cmd(0x4F));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start % 256)));
  DISP_RETURN_ON_ERR(write_data(static_cast<uint8_t>(y_start / 256)));

  // Write pixel data (new RAM plane only; partial uses diff against basemap)
  DISP_RETURN_ON_ERR(write_cmd(0x24));
  return write_data_bytes(data, static_cast<size_t>(height_px) * width_px / 8);
}

// Trigger partial display update.
esp_err_t driver_part_commit() {
  DISP_RETURN_ON_ERR(write_cmd(0x22));
  DISP_RETURN_ON_ERR(write_data(0xFF));
  DISP_RETURN_ON_ERR(write_cmd(0x20));
  wait_busy_low();
  return ESP_OK;
}

// Put SSD1680 into deep sleep mode 1.
esp_err_t driver_deep_sleep() {
  DISP_RETURN_ON_ERR(write_cmd(0x10));
  DISP_RETURN_ON_ERR(write_data(0x01));
  delay_ms(100);
  return ESP_OK;
}

#undef DISP_RETURN_ON_ERR

// ---------------------------------------------------------------------------
// Layout constants (pixel coordinates from UI-IMPLEMENTATION.md §4)
// ---------------------------------------------------------------------------

constexpr int SCREEN_W = 128;
constexpr int CONTENT_W = 122;
constexpr int BODY_Y = 20;
constexpr int BODY_H = 230;

constexpr int STATUS_DIVIDER_Y = 19;

// Home screen: hero blocks
constexpr int PM_BLOCK_Y = 27;
constexpr int PM_BLOCK_H = 44;
constexpr int PM_LABEL_BASELINE_Y = 41;
constexpr int PM_VALUE_BASELINE_Y = 68;
constexpr int CO2_BLOCK_Y = 74;
constexpr int CO2_BLOCK_H = 47;
constexpr int CO2_LABEL_BASELINE_Y = 90;
constexpr int CO2_VALUE_BASELINE_Y = 117;

// Home screen: grid
constexpr int MAIN_DIVIDER_Y = 127;
constexpr int GRID_DIVIDER_X = 61;
constexpr int GRID_TOP_Y = 133;
constexpr int GRID_LINE_1_Y = 162;
constexpr int GRID_LINE_2_Y = 191;
constexpr int GRID_STRONG_LINE_Y = 190;
constexpr int GRID_BOTTOM_Y = 220;

// Home screen: bottom section
constexpr int LOGO_Y = 221;
constexpr int LOGO_H = 24;
constexpr int DISPLAY_OFF_LOGO_Y = 113;
constexpr int DISPLAY_OFF_LOGO_H = 24;

// Chart
constexpr int PLOT_X = 3;
constexpr int PLOT_Y = 225;
constexpr int PLOT_W = 115;
constexpr int PLOT_H = 22;

// Snackbar
constexpr int SNACKBAR_Y = 232;
constexpr int SNACKBAR_H = 18;
constexpr int SNACKBAR_TEXT_BASELINE_Y = 241;

// Menu overlays
constexpr int MAIN_MENU_BG_Y = 128;
constexpr int MAIN_MENU_BG_H = 122;
constexpr int FULL_SCREEN_BG_Y = 24;
constexpr int FULL_SCREEN_BG_H = 226;

// Grid cells (2 columns x 3 rows)
constexpr int CELL_X[6] = {1, 62, 1, 62, 1, 62};
constexpr int CELL_Y[6] = {134, 134, 163, 163, 192, 192};
constexpr int CELL_W = 59;
constexpr int CELL_H = 27;
constexpr int LABEL_X[6] = {10, 68, 10, 68, 10, 68};
constexpr int LABEL_Y[6] = {142, 142, 171, 171, 199, 199};
constexpr int VALUE_X[6] = {10, 68, 10, 68, 10, 68};
constexpr int VALUE_Y[6] = {155, 155, 184, 184, 213, 213};

// ---------------------------------------------------------------------------
// u8g2 virtual display callback — provides geometry info only
// ---------------------------------------------------------------------------

const u8x8_display_info_t U8X8_DISPLAY_INFO_EPD_128X250 = {
    /* chip_enable_level = */ 0,
    /* chip_disable_level = */ 1,
    /* post_chip_enable_wait_ns = */ 0,
    /* pre_chip_disable_wait_ns = */ 0,
    /* reset_pulse_width_ms = */ 0,
    /* post_reset_wait_ms = */ 0,
    /* sda_setup_time_ns = */ 0,
    /* sck_pulse_width_ns = */ 0,
    /* sck_clock_hz = */ 10000000UL,
    /* spi_mode = */ 0,
    /* i2c_bus_clock_100kHz = */ 0,
    /* data_setup_time_ns = */ 0,
    /* write_pulse_width_ns = */ 0,
    /* tile_width = */ 16,
    /* tile_hight = */ 32,
    /* default_x_offset = */ 0,
    /* flipmode_x_offset = */ 0,
    /* pixel_width = */ 128,
    /* pixel_height = */ 250,
};

uint8_t u8x8_d_epd_128x250_cb(u8x8_t *u8x8, uint8_t msg, uint8_t arg_int, void *arg_ptr) {
  (void)arg_int;
  (void)arg_ptr;
  switch (msg) {
  case U8X8_MSG_DISPLAY_SETUP_MEMORY:
    u8x8_d_helper_display_setup_memory(u8x8, &U8X8_DISPLAY_INFO_EPD_128X250);
    return 1;
  default:
    return 1;
  }
}

// ---------------------------------------------------------------------------
// Helper predicates
// ---------------------------------------------------------------------------

bool is_home_like(Screen screen) { return screen == Screen::Home || screen == Screen::MainMenu; }

bool metric_has_chart(Metric metric) { return metric != Metric::None; }

bool is_float_non_negative(float value) { return value >= 0.0f; }

bool is_temperature_valid(float celsius) { return celsius > MeasuresInvalid::TEMPERATURE; }

bool is_altitude_valid(float altitude_m) { return altitude_m > -10000.0f; }

float temp_for_display(float celsius, bool use_fahrenheit) {
  if (!is_temperature_valid(celsius))
    return celsius;
  if (!use_fahrenheit)
    return celsius;
  return celsius * 9.0f / 5.0f + 32.0f;
}

// ---------------------------------------------------------------------------
// Value formatting (inline during rendering)
// ---------------------------------------------------------------------------

int pm25_to_usaqi(float pm25) {
  struct Breakpoint {
    float c_low;
    float c_high;
    int i_low;
    int i_high;
  };
  static const Breakpoint BP[] = {
      {0.0f, 12.0f, 0, 50},       {12.1f, 35.4f, 51, 100},    {35.5f, 55.4f, 101, 150},
      {55.5f, 150.4f, 151, 200},  {150.5f, 250.4f, 201, 300}, {250.5f, 350.4f, 301, 400},
      {350.5f, 500.4f, 401, 500},
  };
  if (!(pm25 >= 0.0f))
    return -1;
  for (const auto &bp : BP) {
    if (pm25 <= bp.c_high) {
      const float ratio = (pm25 - bp.c_low) / (bp.c_high - bp.c_low);
      return bp.i_low + static_cast<int>(lroundf(ratio * static_cast<float>(bp.i_high - bp.i_low)));
    }
  }
  return 500;
}

void format_one_decimal(char *out, size_t out_size, float value) {
  const int scaled = static_cast<int>(lroundf(value * 10.0f));
  const bool neg = scaled < 0;
  const unsigned int abs_scaled = static_cast<unsigned int>(neg ? -scaled : scaled);
  snprintf(out, out_size, "%s%u.%01u", neg ? "-" : "", abs_scaled / 10U, abs_scaled % 10U);
}

void format_pm_value(char *out, size_t out_size, float value, bool use_usaqi) {
  if (!is_float_non_negative(value)) {
    snprintf(out, out_size, "-");
    return;
  }
  if (use_usaqi) {
    const int aqi = pm25_to_usaqi(value);
    if (aqi > 500) {
      snprintf(out, out_size, "500+");
      return;
    }
    snprintf(out, out_size, "%d", aqi);
    return;
  }
  if (value > 999.0f) {
    snprintf(out, out_size, "999+");
    return;
  }
  if (value >= 100.0f) {
    snprintf(out, out_size, "%.0f", value);
    return;
  }
  format_one_decimal(out, out_size, value);
}

void format_co2_value(char *out, size_t out_size, int value) {
  if (value == MeasuresInvalid::CO2) {
    snprintf(out, out_size, "-");
    return;
  }
  if (value > 9999) {
    snprintf(out, out_size, "9999+");
    return;
  }
  snprintf(out, out_size, "%d", value);
}

void format_temperature_value(char *out, size_t out_size, float celsius, bool use_fahrenheit) {
  if (!is_temperature_valid(celsius)) {
    snprintf(out, out_size, "-");
    return;
  }
  const float shown = temp_for_display(celsius, use_fahrenheit);
  snprintf(out, out_size, "%.1f %c", shown, use_fahrenheit ? 'F' : 'C');
}

void format_humidity_value(char *out, size_t out_size, float humidity) {
  if (!is_float_non_negative(humidity)) {
    snprintf(out, out_size, "-");
    return;
  }
  snprintf(out, out_size, "%.0f %%", humidity);
}

// Integer index display for TVOC/NOx grid cells.
void format_int_index_value(char *out, size_t out_size, int value) {
  if (value < 0) {
    snprintf(out, out_size, "-");
    return;
  }
  snprintf(out, out_size, "%d", value);
}

// Float index display for chart min/max stats.
void format_float_index_value(char *out, size_t out_size, float value) {
  if (!is_float_non_negative(value)) {
    snprintf(out, out_size, "-");
    return;
  }
  if (fabsf(value - roundf(value)) < 0.05f) {
    snprintf(out, out_size, "%.0f", value);
    return;
  }
  format_one_decimal(out, out_size, value);
}

void format_pressure_value(char *out, size_t out_size, float pressure_hpa) {
  if (!is_float_non_negative(pressure_hpa)) {
    snprintf(out, out_size, "-");
    return;
  }
  if (pressure_hpa > 9999.0f) {
    snprintf(out, out_size, "9999+ hPa");
    return;
  }
  snprintf(out, out_size, "%.0f hPa", pressure_hpa);
}

void format_altitude_value(char *out, size_t out_size, float altitude_m) {
  if (!is_altitude_valid(altitude_m)) {
    snprintf(out, out_size, "-");
    return;
  }
  if (altitude_m > 9999.0f) {
    snprintf(out, out_size, "9999+ m");
    return;
  }
  snprintf(out, out_size, "%.0f m", altitude_m);
}

void format_chart_stat(char *out, size_t out_size, Metric metric, float value, bool use_fahrenheit,
                       bool pm_use_usaqi) {
  switch (metric) {
  case Metric::Pm25:
    format_pm_value(out, out_size, value, pm_use_usaqi);
    break;
  case Metric::Co2:
    format_co2_value(out, out_size, static_cast<int>(lroundf(value)));
    break;
  case Metric::Temp:
    format_temperature_value(out, out_size, value, use_fahrenheit);
    break;
  case Metric::Humidity:
    format_humidity_value(out, out_size, value);
    break;
  case Metric::Tvoc:
  case Metric::Nox:
    format_float_index_value(out, out_size, value);
    break;
  case Metric::None:
  default:
    snprintf(out, out_size, "-");
    break;
  }
}

// ---------------------------------------------------------------------------
// Drawing primitives
// ---------------------------------------------------------------------------

void draw_text(u8g2_t *u, int x, int baseline_y, const char *text) {
  if (text == nullptr)
    return;
  u8g2_DrawStr(u, static_cast<u8g2_uint_t>(x), static_cast<u8g2_uint_t>(baseline_y), text);
}

void draw_centered_text(u8g2_t *u, int center_x, int baseline_y, const char *text) {
  if (text == nullptr)
    return;
  const int w = static_cast<int>(u8g2_GetStrWidth(u, text));
  draw_text(u, center_x - w / 2, baseline_y, text);
}

void draw_logo(u8g2_t *u, int y, int h) {
  u8g2_SetFont(u, u8g2_font_6x10_tr);
  draw_centered_text(u, CONTENT_W / 2, y + h / 2 + 4, "AirGradient");
}

void draw_lock_icon(u8g2_t *u, int x, int y, bool locked) {
  u8g2_DrawFrame(u, x + 1, y + 5, 7, 6);
  if (locked) {
    u8g2_DrawBox(u, x + 2, y + 6, 5, 4);
  }
  u8g2_DrawCircle(u, x + 4, y + 5, 3, U8G2_DRAW_UPPER_LEFT | U8G2_DRAW_UPPER_RIGHT);
}

// Battery glyphs from u8g2_font_siji_t_6x10
constexpr uint16_t BATTERY_GLYPH_CHARGING = 57914;
constexpr uint16_t BATTERY_GLYPH_LVL_0_10 = 57932;
constexpr uint16_t BATTERY_GLYPH_LVL_10_30 = 57933;
constexpr uint16_t BATTERY_GLYPH_LVL_30_50 = 57935;
constexpr uint16_t BATTERY_GLYPH_LVL_50_70 = 57937;
constexpr uint16_t BATTERY_GLYPH_LVL_70_95 = 57939;
constexpr uint16_t BATTERY_GLYPH_LVL_95_100 = 57940;

uint16_t battery_glyph(bool is_charging, uint8_t pct) {
  if (is_charging)
    return BATTERY_GLYPH_CHARGING;
  const uint8_t v = (pct > 100U) ? 100U : pct;
  if (v <= 10U)
    return BATTERY_GLYPH_LVL_0_10;
  if (v <= 30U)
    return BATTERY_GLYPH_LVL_10_30;
  if (v <= 50U)
    return BATTERY_GLYPH_LVL_30_50;
  if (v <= 70U)
    return BATTERY_GLYPH_LVL_50_70;
  if (v <= 95U)
    return BATTERY_GLYPH_LVL_70_95;
  return BATTERY_GLYPH_LVL_95_100;
}

// Draw a single grid cell with optional inverted highlight.
void draw_cell(u8g2_t *u, int index, const char *label, const char *value, bool selected) {
  if (selected) {
    u8g2_DrawBox(u, CELL_X[index], CELL_Y[index], CELL_W, CELL_H);
    u8g2_SetDrawColor(u, 1);
  }
  u8g2_SetFont(u, u8g2_font_6x10_tr);
  draw_text(u, LABEL_X[index], LABEL_Y[index], label);
  draw_text(u, VALUE_X[index], VALUE_Y[index], value);
  if (selected) {
    u8g2_SetDrawColor(u, 0);
  }
}

// Draw list/menu rows with selection highlight.
void draw_list_rows(u8g2_t *u, const DisplayValues &v, bool full_screen) {
  const int row_base_y = full_screen ? 25 : 132;
  constexpr int ROW_RECT_X = 5;
  constexpr int ROW_RECT_W = 112;
  constexpr int ROW_RECT_H = 20;
  constexpr int ROW_STEP = 22;
  const int text_baseline = full_screen ? 35 : 142;

  for (uint8_t i = 0; i < v.row_count && i < MAX_LIST_ROWS; ++i) {
    const int row_y = row_base_y + ROW_STEP * static_cast<int>(i);
    const bool selected = (i == v.selected_row) && !v.rows[i].disabled;
    if (selected) {
      u8g2_DrawBox(u, ROW_RECT_X, row_y, ROW_RECT_W, ROW_RECT_H);
      u8g2_SetDrawColor(u, 1);
    }
    u8g2_SetFont(u, u8g2_font_6x10_tr);
    draw_text(u, 10, text_baseline + ROW_STEP * static_cast<int>(i), v.rows[i].text);
    if (selected) {
      u8g2_SetDrawColor(u, 0);
    }
  }

  if (v.show_separator_after_back) {
    u8g2_DrawHLine(u, 7, 69, 108);
  }
}

} // anonymous namespace

// ===========================================================================
// DisplayService — public methods
// ===========================================================================

DisplayService::DisplayService(const Config &config)
    : _config(config), _u8g2{}, _render_buf{}, _spi_buf{}, _region_buf{}, _prev_values{},
      _partial_count(0), _pending_full(false), _task_handle(nullptr), _running(false),
      _worker_busy(false) {}

bool DisplayService::init(const DisplayValues &initial) {
  esp_err_t err = driver_init(_config);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "driver init failed: %s", esp_err_to_name(err));
    return false;
  }

  // u8g2: software renderer into RAM only — no hardware callbacks
  u8g2_SetupDisplay(&_u8g2, u8x8_d_epd_128x250_cb, u8x8_dummy_cb, u8x8_dummy_cb, u8x8_dummy_cb);
  u8g2_SetupBuffer(&_u8g2, _render_buf, BUF_TILE_HEIGHT, u8g2_ll_hvline_horizontal_right_lsb,
                   U8G2_MIRROR);
  u8g2_SetFontMode(&_u8g2, 1); // Transparent font mode

  _prev_values = initial;
  _render_frame(initial);

  // Synchronous initial full refresh (worker not yet started)
  err = driver_bus_acquire();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bus acquire failed for init: %s", esp_err_to_name(err));
    return false;
  }
  err = driver_hw_init_full();
  if (err == ESP_OK) {
    err = driver_set_basemap(_render_buf);
  }
  driver_bus_release();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "initial refresh failed: %s", esp_err_to_name(err));
    return false;
  }
  _partial_count = 0;

  // Start async worker task.
  _running = true;
  const bool created = RTOS::task_create(
      _worker_entry, "disp_worker", static_cast<uint32_t>(_config.task_stack_size), this,
      static_cast<uint32_t>(_config.task_priority), &_task_handle);
  if (!created) {
    ESP_LOGE(TAG, "failed to create worker task");
    _running = false;
    _task_handle = nullptr;
    return false;
  }

  return true;
}

bool DisplayService::update(const DisplayValues &values, bool wait) {
  if (_task_handle == nullptr)
    return false;

  if (_worker_busy) {
    if (!wait)
      return false;
    while (_worker_busy) {
      RTOS::delay_ms(1);
    }
  }

  const bool header_changed = _is_header_changed(values, _prev_values);
  const bool can_partial =
      is_home_like(_prev_values.screen) && is_home_like(values.screen) && !header_changed;

  _render_frame(values);

  _pending_full = !can_partial || _partial_count >= _config.max_partial_ops;

  memcpy(_spi_buf, _render_buf, sizeof(_render_buf));
  _worker_busy = true;
  _prev_values = values;

  RTOS::task_notify_give(_task_handle);
  return true;
}

void DisplayService::update_sync(const DisplayValues &values) {
  _render_frame(values);

  esp_err_t err = driver_bus_acquire();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bus acquire failed for sync update: %s", esp_err_to_name(err));
    return;
  }
  err = driver_hw_init_full();
  if (err == ESP_OK) {
    err = driver_set_basemap(_render_buf);
  }
  driver_bus_release();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "sync update failed: %s", esp_err_to_name(err));
  }
  _partial_count = 0;
  _prev_values = values;
}

void DisplayService::clear() {
  memset(_render_buf, 0xFF, sizeof(_render_buf));

  // Wait for worker to finish if active
  while (_worker_busy) {
    RTOS::delay_ms(1);
  }

  memcpy(_spi_buf, _render_buf, sizeof(_render_buf));

  esp_err_t err = driver_bus_acquire();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bus acquire failed for clear: %s", esp_err_to_name(err));
    return;
  }
  err = driver_hw_init_full();
  if (err == ESP_OK) {
    err = driver_set_basemap(_spi_buf);
  }
  driver_bus_release();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "clear failed: %s", esp_err_to_name(err));
  }
  _partial_count = 0;
}

void DisplayService::deep_sleep() {
  esp_err_t err = driver_bus_acquire();
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "bus acquire failed for deep sleep: %s", esp_err_to_name(err));
    return;
  }
  err = driver_deep_sleep();
  driver_bus_release();

  if (err != ESP_OK) {
    ESP_LOGE(TAG, "deep sleep failed: %s", esp_err_to_name(err));
  }
}

void DisplayService::stop() {
  if (_task_handle == nullptr)
    return;
  _running = false;
  RTOS::task_notify_give(_task_handle); // Wake worker so it can exit

  // Wait for worker to finish current operation
  while (_worker_busy) {
    RTOS::delay_ms(1);
  }
  RTOS::delay_ms(50); // Let worker task fully exit
  _task_handle = nullptr;
}

// ===========================================================================
// DisplayService — rendering pipeline
// ===========================================================================

void DisplayService::_render_frame(const DisplayValues &v) {
  memset(_render_buf, 0xFF, sizeof(_render_buf));
  u8g2_SetDrawColor(&_u8g2, 0); // Black on white

  if (v.screen == Screen::Shutdown) {
    _draw_shutdown();
    _draw_snackbar(v);
    return;
  }

  _draw_status_bar(v);

  switch (v.screen) {
  case Screen::Home:
    _draw_home(v);
    break;
  case Screen::MainMenu:
    _draw_home(v);
    _draw_menu_overlay(v);
    break;
  case Screen::Settings:
  case Screen::SettingsChoice:
  case Screen::TagList:
  case Screen::Confirm:
  case Screen::About:
    _draw_full_screen_list(v);
    break;
  case Screen::Shutdown:
    break; // Already handled above
  }

  _draw_snackbar(v);
}

bool DisplayService::_is_header_changed(const DisplayValues &a, const DisplayValues &b) const {
  return a.hour != b.hour || a.minute != b.minute || a.battery_pct != b.battery_pct ||
         a.is_battery_charging != b.is_battery_charging || a.locked != b.locked ||
         a.ble_enabled != b.ble_enabled || a.ble_connected != b.ble_connected ||
         a.wifi_enabled != b.wifi_enabled || a.gps_enabled != b.gps_enabled ||
         a.gps_fix != b.gps_fix || a.tracking_active != b.tracking_active;
}

// ===========================================================================
// DisplayService — screen draw methods
// ===========================================================================

void DisplayService::_draw_status_bar(const DisplayValues &v) {
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  draw_lock_icon(&_u8g2, 3, 2, v.locked);

  if (v.ble_enabled) {
    draw_text(&_u8g2, 10, 10, "BLE");
    if (v.ble_connected) {
      u8g2_DrawDisc(&_u8g2, 26, 6, 1, U8G2_DRAW_ALL);
    }
  }
  if (v.wifi_enabled) {
    draw_text(&_u8g2, 30, 10, "WiFi");
  }
  if (v.gps_enabled) {
    draw_text(&_u8g2, 56, 10, "GPS");
    if (v.gps_fix) {
      u8g2_DrawDisc(&_u8g2, 75, 6, 1, U8G2_DRAW_ALL);
    }
  }
  if (v.tracking_active) {
    u8g2_DrawDisc(&_u8g2, 98, 10, 2, U8G2_DRAW_ALL);
  }

  if (v.battery_pct != 0xFFu || v.is_battery_charging) {
    u8g2_SetFont(&_u8g2, u8g2_font_siji_t_6x10);
    u8g2_DrawGlyph(&_u8g2, 100, 11, battery_glyph(v.is_battery_charging, v.battery_pct));
  }

  u8g2_DrawHLine(&_u8g2, 0, STATUS_DIVIDER_Y, CONTENT_W);
}

void DisplayService::_draw_home(const DisplayValues &v) {
  if (v.display_off) {
    draw_logo(&_u8g2, DISPLAY_OFF_LOGO_Y, DISPLAY_OFF_LOGO_H);
    return;
  }

  const bool chart_visible = metric_has_chart(v.active_metric);
  const bool pm_selected = (v.active_metric == Metric::Pm25);
  const bool co2_selected = (v.active_metric == Metric::Co2);

  // Highlight selected hero block (inverted)
  if (pm_selected) {
    u8g2_DrawBox(&_u8g2, 0, PM_BLOCK_Y, CONTENT_W, PM_BLOCK_H);
  }
  if (co2_selected) {
    u8g2_DrawBox(&_u8g2, 0, CO2_BLOCK_Y, CONTENT_W, CO2_BLOCK_H);
  }

  // Main divider (double line)
  u8g2_DrawHLine(&_u8g2, 0, MAIN_DIVIDER_Y, CONTENT_W);
  u8g2_DrawHLine(&_u8g2, 0, MAIN_DIVIDER_Y + 1, CONTENT_W);

  // Grid structure
  u8g2_DrawVLine(&_u8g2, GRID_DIVIDER_X, GRID_TOP_Y, 86);
  u8g2_DrawHLine(&_u8g2, 0, GRID_LINE_1_Y, CONTENT_W);
  if (!chart_visible) {
    u8g2_DrawHLine(&_u8g2, 0, GRID_LINE_2_Y, CONTENT_W);
    u8g2_DrawHLine(&_u8g2, 0, GRID_BOTTOM_Y, CONTENT_W);
  }

  char buf[24];

  // PM2.5 hero block
  const char *pm_label = v.pm_use_usaqi ? "PM2.5 (USAQI)" : "PM2.5 (ug/m3)";
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  if (pm_selected)
    u8g2_SetDrawColor(&_u8g2, 1);
  draw_centered_text(&_u8g2, CONTENT_W / 2, PM_LABEL_BASELINE_Y, pm_label);
  u8g2_SetFont(&_u8g2, u8g2_font_10x20_tn);
  format_pm_value(buf, sizeof(buf), v.pm25_ugm3, v.pm_use_usaqi);
  draw_centered_text(&_u8g2, CONTENT_W / 2, PM_VALUE_BASELINE_Y, buf);
  if (pm_selected)
    u8g2_SetDrawColor(&_u8g2, 0);

  // CO2 hero block
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  if (co2_selected)
    u8g2_SetDrawColor(&_u8g2, 1);
  draw_centered_text(&_u8g2, CONTENT_W / 2, CO2_LABEL_BASELINE_Y, "CO2 (ppm)");
  u8g2_SetFont(&_u8g2, u8g2_font_10x20_tn);
  format_co2_value(buf, sizeof(buf), v.co2_ppm);
  draw_centered_text(&_u8g2, CONTENT_W / 2, CO2_VALUE_BASELINE_Y, buf);
  if (co2_selected)
    u8g2_SetDrawColor(&_u8g2, 0);

  // Grid cells
  char temp_buf[24];
  char hum_buf[24];
  char tvoc_buf[24];
  char nox_buf[24];
  char left_bottom_buf[24];
  char right_bottom_buf[24];

  format_temperature_value(temp_buf, sizeof(temp_buf), v.temperature_c, v.use_fahrenheit);
  format_humidity_value(hum_buf, sizeof(hum_buf), v.humidity_pct);
  format_int_index_value(tvoc_buf, sizeof(tvoc_buf), v.tvoc_index);
  format_int_index_value(nox_buf, sizeof(nox_buf), v.nox_index);

  const char *left_bottom_label = "Pressure";
  const char *right_bottom_label = "Altitude";
  if (chart_visible) {
    left_bottom_label = "Min";
    right_bottom_label = "Max";
    format_chart_stat(left_bottom_buf, sizeof(left_bottom_buf), v.active_metric, v.chart_min,
                      v.use_fahrenheit, v.pm_use_usaqi);
    format_chart_stat(right_bottom_buf, sizeof(right_bottom_buf), v.active_metric, v.chart_max,
                      v.use_fahrenheit, v.pm_use_usaqi);
  } else {
    format_pressure_value(left_bottom_buf, sizeof(left_bottom_buf), v.pressure_hpa);
    format_altitude_value(right_bottom_buf, sizeof(right_bottom_buf), v.altitude_m);
  }

  draw_cell(&_u8g2, 0, "Temp", temp_buf, v.active_metric == Metric::Temp);
  draw_cell(&_u8g2, 1, "Humidity", hum_buf, v.active_metric == Metric::Humidity);
  draw_cell(&_u8g2, 2, "TVOC", tvoc_buf, v.active_metric == Metric::Tvoc);
  draw_cell(&_u8g2, 3, "NOx", nox_buf, v.active_metric == Metric::Nox);
  draw_cell(&_u8g2, 4, left_bottom_label, left_bottom_buf, false);
  draw_cell(&_u8g2, 5, right_bottom_label, right_bottom_buf, false);

  if (chart_visible) {
    _draw_chart(v);
  } else {
    draw_logo(&_u8g2, LOGO_Y, LOGO_H);
  }
}

void DisplayService::_draw_menu_overlay(const DisplayValues &v) {
  // White overlay area (erase underlying home screen content)
  u8g2_DrawBox(&_u8g2, 0, MAIN_MENU_BG_Y, CONTENT_W, MAIN_MENU_BG_H);
  u8g2_SetDrawColor(&_u8g2, 1);
  u8g2_DrawBox(&_u8g2, 0, MAIN_MENU_BG_Y, CONTENT_W, MAIN_MENU_BG_H);
  u8g2_SetDrawColor(&_u8g2, 0);
  draw_list_rows(&_u8g2, v, false);
}

void DisplayService::_draw_full_screen_list(const DisplayValues &v) {
  // White overlay area (full-screen list background)
  u8g2_DrawBox(&_u8g2, 0, FULL_SCREEN_BG_Y, CONTENT_W, FULL_SCREEN_BG_H);
  u8g2_SetDrawColor(&_u8g2, 1);
  u8g2_DrawBox(&_u8g2, 0, FULL_SCREEN_BG_Y, CONTENT_W, FULL_SCREEN_BG_H);
  u8g2_SetDrawColor(&_u8g2, 0);
  draw_list_rows(&_u8g2, v, true);

  // About screen has additional text fields below the list rows
  if (v.screen == Screen::About) {
    u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
    draw_text(&_u8g2, 10, 91, v.about_title);
    draw_text(&_u8g2, 10, 106, v.about_firmware);
    draw_text(&_u8g2, 10, 120, v.about_serial);
    draw_text(&_u8g2, 10, 133, v.about_hardware);
  }
}

void DisplayService::_draw_snackbar(const DisplayValues &v) {
  if (v.snackbar_text == nullptr || v.snackbar_text[0] == '\0')
    return;

  u8g2_DrawBox(&_u8g2, 0, SNACKBAR_Y, CONTENT_W, SNACKBAR_H);
  u8g2_SetDrawColor(&_u8g2, 1);
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  draw_centered_text(&_u8g2, CONTENT_W / 2, SNACKBAR_TEXT_BASELINE_Y, v.snackbar_text);
  u8g2_SetDrawColor(&_u8g2, 0);
}

void DisplayService::_draw_shutdown() {
  u8g2_SetFont(&_u8g2, u8g2_font_6x10_tr);
  draw_centered_text(&_u8g2, CONTENT_W / 2, 115, "Powering off...");
  draw_centered_text(&_u8g2, CONTENT_W / 2, 135, "See you soon");
  draw_logo(&_u8g2, 218, 24);
}

void DisplayService::_draw_chart(const DisplayValues &v) {
  // Chart border lines
  u8g2_DrawHLine(&_u8g2, 0, GRID_STRONG_LINE_Y, CONTENT_W);
  u8g2_DrawHLine(&_u8g2, 0, GRID_STRONG_LINE_Y + 1, CONTENT_W);
  u8g2_DrawHLine(&_u8g2, 0, GRID_BOTTOM_Y, CONTENT_W);

  // Chart axes
  u8g2_DrawVLine(&_u8g2, PLOT_X, PLOT_Y, PLOT_H);
  u8g2_DrawHLine(&_u8g2, PLOT_X, PLOT_Y + PLOT_H, PLOT_W);

  if (v.chart_samples == nullptr || v.chart_count == 0)
    return;

  const float range = v.chart_max - v.chart_min;
  int prev_x = PLOT_X;
  int prev_y = PLOT_Y + PLOT_H / 2;

  for (int x = 0; x < PLOT_W; ++x) {
    const int sample_index =
        (x * static_cast<int>(v.chart_count - 1)) / ((PLOT_W > 1) ? (PLOT_W - 1) : 1);
    const float sample = v.chart_samples[sample_index];

    int y = PLOT_Y + PLOT_H / 2;
    if (range > 0.001f) {
      const float norm = (sample - v.chart_min) / range;
      y = PLOT_Y + static_cast<int>(lroundf((1.0f - norm) * static_cast<float>(PLOT_H)));
    }
    if (y < PLOT_Y)
      y = PLOT_Y;
    if (y > PLOT_Y + PLOT_H)
      y = PLOT_Y + PLOT_H;

    const int draw_x = PLOT_X + x;
    if (x > 0) {
      u8g2_DrawLine(&_u8g2, prev_x, prev_y, draw_x, y);
    }
    prev_x = draw_x;
    prev_y = y;
  }
}

// ===========================================================================
// DisplayService — async worker task
// ===========================================================================

void DisplayService::_worker_entry(void *arg) {
  auto *self = static_cast<DisplayService *>(arg);
  self->_worker_loop();
  RTOS::task_delete(nullptr);
}

void DisplayService::_worker_loop() {
  while (_running) {
    // Block until the orchestrator signals frame-ready.
    RTOS::task_notify_take(UINT32_MAX);
    if (!_running)
      break;

    esp_err_t err = driver_bus_acquire();
    if (err != ESP_OK) {
      ESP_LOGE(TAG, "worker: bus acquire failed: %s", esp_err_to_name(err));
      _worker_busy = false;
      continue;
    }

    if (_pending_full) {
      err = driver_hw_init_full();
      if (err == ESP_OK) {
        err = driver_set_basemap(_spi_buf);
      }
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "worker: full update failed: %s", esp_err_to_name(err));
      }
      _partial_count = 0;
    } else {
      err = driver_part_begin();
      if (err == ESP_OK) {
        // Extract body region from SPI buffer
        memcpy(_region_buf, _spi_buf + BODY_Y * BUF_ROW_BYTES, BODY_H * BUF_ROW_BYTES);
        err = driver_part_write_region(0, BODY_Y, _region_buf, BODY_H, SCREEN_W);
      }
      if (err == ESP_OK) {
        err = driver_part_commit();
      }
      if (err != ESP_OK) {
        ESP_LOGE(TAG, "worker: partial update failed: %s", esp_err_to_name(err));
      }
      _partial_count++;
    }

    driver_bus_release();
    _worker_busy = false;
  }
}

#endif // !TEST_HOST
