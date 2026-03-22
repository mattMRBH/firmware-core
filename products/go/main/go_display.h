#ifndef GO_DISPLAY_H
#define GO_DISPLAY_H

#include <cstdint>

#include "measures_types.h"
#include "rtos.h"

// ---------------------------------------------------------------------------
// Host-compatible types (no ESP-IDF dependency)
// ---------------------------------------------------------------------------

enum class Screen : uint8_t {
  Home,
  MainMenu,
  Settings,
  SettingsChoice,
  TagList,
  About,
  Confirm,
  Shutdown,
};

enum class Metric : uint8_t {
  None,
  Pm25,
  Co2,
  Temp,
  Humidity,
  Tvoc,
  Nox,
};

static constexpr uint8_t MAX_LIST_ROWS = 9;

struct ListRow {
  char text[48];
  bool disabled = false;
};

struct DisplayValues {
  // --- Sensor readings (channel A) ---
  int co2_ppm = MeasuresInvalid::CO2;
  float pm25_ugm3 = MeasuresInvalid::PM;
  float temperature_c = MeasuresInvalid::TEMPERATURE;
  float humidity_pct = MeasuresInvalid::HUMIDITY;
  int tvoc_index = MeasuresInvalid::TVOC;
  int nox_index = MeasuresInvalid::NOX;
  float pressure_hpa = MeasuresInvalid::PM;
  float altitude_m = MeasuresInvalid::PM;

  // --- Clock (from GPS) ---
  uint8_t hour = 0xFF;   // 0xFF = no data
  uint8_t minute = 0xFF; // 0xFF = no data

  // --- Battery ---
  uint8_t battery_pct = 0xFF; // 0xFF = no data
  bool is_battery_charging = false;

  // --- Status flags ---
  bool locked = true;
  bool ble_enabled = false;
  bool ble_connected = false;
  bool wifi_enabled = false;
  bool gps_enabled = true;
  bool gps_fix = false;
  bool tracking_active = false;
  bool display_off = false;
  bool use_fahrenheit = false;
  bool pm_use_usaqi = false;

  // --- Screen navigation ---
  Screen screen = Screen::Home;
  Metric active_metric = Metric::None;

  // --- List/menu content ---
  ListRow rows[MAX_LIST_ROWS] = {};
  uint8_t row_count = 0;
  uint8_t selected_row = 0;
  bool show_separator_after_back = false;

  // --- About screen ---
  const char *about_title = nullptr;
  const char *about_firmware = nullptr;
  const char *about_serial = nullptr;
  const char *about_hardware = nullptr;

  // --- Chart data ---
  const float *chart_samples = nullptr;
  uint8_t chart_count = 0;
  float chart_min = 0.0f;
  float chart_max = 0.0f;

  // --- Snackbar ---
  const char *snackbar_text = nullptr;
};

// ---------------------------------------------------------------------------
// DisplayService (hardware-dependent, excluded from host builds)
// ---------------------------------------------------------------------------

#ifndef TEST_HOST

#include <driver/spi_master.h>

extern "C" {
#include "u8g2.h"
}

class DisplayService {
public:
  struct Config {
    // SPI
    spi_host_device_t spi_host;
    int pin_cs;
    int pin_dc;
    int pin_rst;
    int pin_busy;
    int clock_hz = 4000000;
    int bus_acquire_timeout_ms = 1000;

    // Worker task
    uint16_t task_stack_size = 4096;
    uint8_t task_priority = 4;

    // Refresh limits
    uint8_t max_partial_ops = 20;
  };

  explicit DisplayService(const Config &config);

  /// Initialize display hardware and start worker task.
  /// Performs a full refresh with the initial values.
  bool init(const DisplayValues &initial);

  /// Submit a new frame for display.
  /// Renders into framebuffer (fast), then signals worker task.
  /// wait=false and worker busy: returns false (skipped).
  /// wait=true: blocks until worker finishes previous refresh.
  bool update(const DisplayValues &values, bool wait = false);

  /// Synchronous one-shot update for fast-path boot.
  /// Renders and drives SPI inline (blocking). Does not use worker task.
  void update_sync(const DisplayValues &values);

  /// Clear display to white (full refresh). Blocking.
  void clear();

  /// Put EPD controller into deep sleep mode.
  void deep_sleep();

  /// Stop worker task. Call before entering ESP deep sleep.
  void stop();

private:
  static constexpr int BUF_ROW_BYTES = 16;
  static constexpr int BUF_TILE_HEIGHT = 32;
  static constexpr int BUF_SIZE = BUF_ROW_BYTES * BUF_TILE_HEIGHT * 8; // 4096
  static constexpr int BODY_Y = 20;
  static constexpr int BODY_H = 230;
  static constexpr int REGION_SIZE = BUF_ROW_BYTES * BODY_H; // 3680

  Config _config;

  // u8g2 instance and render buffer
  u8g2_t _u8g2;
  uint8_t _render_buf[BUF_SIZE];

  // SPI transmit buffer (owned by worker after signal)
  uint8_t _spi_buf[BUF_SIZE];
  uint8_t _region_buf[REGION_SIZE];

  // Refresh state
  DisplayValues _prev_values;
  uint8_t _partial_count = 0;
  bool _pending_full = false;

  // Worker task
  RtosTaskHandle _task_handle = nullptr;
  volatile bool _running = false;
  volatile bool _worker_busy = false;

  // Render methods
  void _render_frame(const DisplayValues &v);
  bool _is_header_changed(const DisplayValues &a, const DisplayValues &b) const;

  void _draw_status_bar(const DisplayValues &v);
  void _draw_home(const DisplayValues &v);
  void _draw_menu_overlay(const DisplayValues &v);
  void _draw_full_screen_list(const DisplayValues &v);
  void _draw_snackbar(const DisplayValues &v);
  void _draw_shutdown();
  void _draw_chart(const DisplayValues &v);

  // Worker
  static void _worker_entry(void *arg);
  void _worker_loop();
};

#endif // !TEST_HOST

#endif // GO_DISPLAY_H
