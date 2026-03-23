/**
 * AirGradient Go — Application Entry Point
 *
 * Boot path selection, hardware initialization, and fast-path boot for
 * the AirGradient Go portable air quality monitor.
 *
 * Three boot paths:
 *   1. Fresh power-on   -> full initialization with default state
 *   2. Timer wake        -> fast-path: measure, display, sleep (no event loop)
 *   3. Button wake       -> full initialization, restore state, unlock
 *
 * All objects are stack-allocated in the boot function they belong to.
 * Since the orchestrator's run() never returns, they live for the duration
 * of the program.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <ctime>

#include <driver/gpio.h>
#include <driver/i2c_master.h>
#include <driver/spi_master.h>
#include <esp_mac.h>
#include <nvs_flash.h>

#include "ag_log.h"
#include "airgradient_uart.h"
#include "backends/rtc_payload_cache_storage.h"
#include "cap1203.h"
#include "drivers/bq25xx/bq25xx.h"
#include "drivers/nmea_gps/nmea_gps.h"
#include "drivers/pms5003/pms5003.h"
#include "drivers/s8/s8.h"
#include "drivers/sgp41/sgp41.h"
#include "drivers/sht40/sht40.h"
#include "native_gpio.h"
#include "nvs_config_store.h"
#include "rtos.h"
#include "services/payload_cache.h"
#include "services/sensor_manager.h"
#include "spi_nand_storage.h"

#include "board_config.h"
#include "go_display.h"
#include "go_events.h"
#include "go_gps.h"
#include "go_input.h"
#include "go_orchestrator.h"
#include "go_power.h"
#include "go_sensor_producer.h"
#include "go_settings.h"
#include "go_storage.h"
#include "go_types.h"
#include "go_ui.h"

static constexpr const char *TAG = "main";

static constexpr const char *FIRMWARE_VERSION = "0.1.0";

// ---------------------------------------------------------------------------
// Forward declarations
// ---------------------------------------------------------------------------

static void run_fast_path(const RtcAppState &state);
static void run_full_boot(WakeCause cause);

static void init_nvs();
static i2c_master_bus_handle_t init_i2c_bus();
static void init_gpio();
static void init_spi_buses();
static const char *get_serial_number();
static MeasuresAGo measures_to_ago(const Measures &m);
static DisplayValues build_fast_path_display(const Measures &measures, const GpsData &gps,
                                             const PowerSnapshot &bms, const GoSettings &settings);
static uint32_t compute_fast_path_sleep_duration(const GoSettings &settings);

// ===========================================================================
// app_main — boot path selection
// ===========================================================================

extern "C" void app_main() {
  WakeCause cause = PowerService::get_wake_cause();

  if (cause == WakeCause::Timer) {
    RtcAppState state = load_rtc_app_state();
    if (PowerService::is_fast_path_wake(cause, state)) {
      run_fast_path(state);
      // Never returns.
    }
  }

  run_full_boot(cause);
  // Never returns.
}

// ===========================================================================
// Fast-path boot (timer wake, locked)
//
// Minimal initialization — no event loop, no producer tasks, no input
// handling.  Goal: measure, display, sleep — as fast as possible to
// conserve battery.
// ===========================================================================

static void run_fast_path(const RtcAppState &state) {
  AG_LOGI(TAG, "run_fast_path: entering fast-path boot");

  // --- 0. NVS + settings (needed for sleep duration, GPS mode) ---
  init_nvs();
  NvsConfigStore config_store("go");
  GoSettings settings = load_go_settings(config_store);

  // --- 1. GPIO (sensor power enables) ---
  init_gpio();
  RTOS::delay_ms(100);

  // --- 2. I2C bus ---
  i2c_master_bus_handle_t i2c_bus = init_i2c_bus();
  RTOS::delay_ms(100);

  // --- 3. SPI buses (needed for NAND and display) ---
  init_spi_buses();

  // --- 4. Sensor drivers (same construction as full boot) ---
  SHT40 sht40(i2c_bus, I2C_ADDR_SHT40);
  SGP41 sgp41(i2c_bus, I2C_ADDR_SGP41);

  AirgradientUART co2_serial(UART_PORT_CO2, PIN_CO2_RX, PIN_CO2_TX);
  co2_serial.begin(CO2_BAUD);
  S8 co2(co2_serial);

  AirgradientUART pm_serial(UART_PORT_PM, PIN_PM_RX, PIN_PM_TX);
  pm_serial.begin(PM_BAUD);
  PMS5003 pms(pm_serial);

  Sensors sensors{};

  if (sht40.init()) {
    sensors.temp_hum = &sht40;
  } else {
    AG_LOGE(TAG, "SHT40 init failed");
  }

  if (sgp41.init()) {
    sensors.tvoc_nox = &sgp41;
  } else {
    AG_LOGE(TAG, "SGP41 init failed");
  }

  if (co2.init()) {
    sensors.co2 = &co2;
  } else {
    AG_LOGE(TAG, "CO2 sensor init failed");
  }

  if (pms.init()) {
    sensors.pms_a = &pms;
  } else {
    AG_LOGE(TAG, "PMS5003 init failed");
  }

  SensorManager sensor_manager(sensors);

  // --- 5. One-shot measurement (blocking, single iteration) ---
  Measures measures = sensor_manager.start_measures(1);

  // --- 6. One-shot GPS (if tracking + GPS active) ---
  GpsData gps{};
  const bool gps_active = (settings.gps_mode == GpsMode::AlwaysOn) ||
                          (settings.gps_mode == GpsMode::OnWhenTracking && state.tracking_active);

  if (state.tracking_active && gps_active) {
    AirgradientUART gps_serial(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
    NmeaGps nmea_gps(gps_serial);
    gps = gps_read_once(nmea_gps, GPS_BAUD, 2000);
  }

  // --- 7. Storage ---
  RtcPayloadCacheStorage rtc_storage;
  PayloadCache cache(rtc_storage, PAYLOAD_CACHE_MAX_SIZE);

  SpiNandStorage::Config nand_config{};
  nand_config.spi_host = SPI_HOST_NAND;
  nand_config.cs_pin = static_cast<gpio_num_t>(PIN_NAND_CS);
  SpiNandStorage nand(nand_config);

  StorageService storage(cache, nand);
  storage.restore_cache();
  if (!storage.init()) {
    AG_LOGE(TAG, "NAND storage init failed (fast path)");
  }

  MeasuresAGo ago = measures_to_ago(measures);
  storage.cache_measurement(ago);

  if (state.tracking_active) {
    storage.start_route(state.tracking_session_id);
    RoutePoint point{};
    point.timestamp = time(nullptr);
    point.gps = gps;
    point.sensors = ago;
    storage.append_route_point(point);
    storage.end_route();
  }

  storage.backup_cache();

  // --- 8. BMS (poll for display + watchdog reset) ---
  BQ25XX bms(i2c_bus, I2C_ADDR_BMS);
  if (!bms.init()) {
    AG_LOGE(TAG, "BMS init failed (fast path)");
  }

  PowerService power_service(bms, gpio::native::hal,
                             {
                                 .pin_wake_button_power = PIN_BUTTON_POWER,
                                 .pin_wake_button_boot = PIN_BUTTON_BOOT,
                             });

  PowerSnapshot bms_snap = power_service.poll_bms();
  power_service.reset_watchdog();

  // --- 9. Display (synchronous, no worker task) ---
  DisplayService display({
      .spi_host = SPI_HOST_DISPLAY,
      .pin_cs = PIN_DISPLAY_CS,
      .pin_dc = PIN_DISPLAY_DC,
      .pin_rst = PIN_DISPLAY_RST,
      .pin_busy = PIN_DISPLAY_BUSY,
  });

  DisplayValues values = build_fast_path_display(measures, gps, bms_snap, settings);
  display.update_sync(values);

  // --- 10. Save state and re-enter deep sleep ---
  power_service.save_state(state);
  uint32_t next_ms = compute_fast_path_sleep_duration(settings);
  power_service.enter_sleep(PowerService::SleepType::Deep, next_ms);
  // Never returns — CPU reboots on wake.
}

// ===========================================================================
// Full boot (fresh power-on or button wake)
//
// Initializes all hardware and services in dependency order, then hands
// control to the Orchestrator.
// ===========================================================================

static void run_full_boot(WakeCause cause) {
  AG_LOGI(TAG, "run_full_boot: cause=%d", static_cast<int>(cause));

  // --- 1. NVS ---
  init_nvs();

  // --- 2. Settings ---
  NvsConfigStore config_store("go");
  GoSettings settings = load_go_settings(config_store);

  // --- 3. GPIO (power enables, initial levels) ---
  init_gpio();
  RTOS::delay_ms(100);

  // --- 4. I2C bus ---
  i2c_master_bus_handle_t i2c_bus = init_i2c_bus();
  RTOS::delay_ms(100);

  // --- 5. SPI bus(es) ---
  init_spi_buses();

  // --- 6. Sensor drivers ---
  SHT40 sht40(i2c_bus, I2C_ADDR_SHT40);
  SGP41 sgp41(i2c_bus, I2C_ADDR_SGP41);

  AirgradientUART co2_serial(UART_PORT_CO2, PIN_CO2_RX, PIN_CO2_TX);
  co2_serial.begin(CO2_BAUD);
  S8 co2(co2_serial);

  AirgradientUART pm_serial(UART_PORT_PM, PIN_PM_RX, PIN_PM_TX);
  pm_serial.begin(PM_BAUD);
  PMS5003 pms(pm_serial);

  Sensors sensors{};

  if (sht40.init()) {
    sensors.temp_hum = &sht40;
  } else {
    AG_LOGE(TAG, "SHT40 init failed");
  }

  if (sgp41.init()) {
    sensors.tvoc_nox = &sgp41;
  } else {
    AG_LOGE(TAG, "SGP41 init failed");
  }

  if (co2.init()) {
    sensors.co2 = &co2;
  } else {
    AG_LOGE(TAG, "CO2 sensor init failed");
  }

  if (pms.init()) {
    sensors.pms_a = &pms;
  } else {
    AG_LOGE(TAG, "PMS5003 init failed");
  }

  // --- 7. Sensors struct + SensorManager ---
  SensorManager sensor_manager(sensors);

  // --- 8. BMS ---
  BQ25XX bms(i2c_bus, I2C_ADDR_BMS);
  if (!bms.init()) {
    AG_LOGE(TAG, "BMS init failed");
  }

  // --- 9. GPS ---
  AirgradientUART gps_serial(UART_PORT_GPS, PIN_GPS_RX, PIN_GPS_TX);
  gps_serial.begin(GPS_BAUD);
  NmeaGps nmea_gps(gps_serial);

  // --- 10. Touch ---
  CAP1203 touch(i2c_bus, I2C_ADDR_CAP1203);
  if (!touch.init()) {
    AG_LOGE(TAG, "CAP1203 touch init failed");
  }

  // --- 11. Storage ---
  RtcPayloadCacheStorage rtc_storage;
  PayloadCache cache(rtc_storage, PAYLOAD_CACHE_MAX_SIZE);

  SpiNandStorage::Config nand_config{};
  nand_config.spi_host = SPI_HOST_NAND;
  nand_config.cs_pin = static_cast<gpio_num_t>(PIN_NAND_CS);
  SpiNandStorage nand(nand_config);

  StorageService storage(cache, nand);
  storage.restore_cache();
  if (!storage.init()) {
    AG_LOGE(TAG, "NAND storage init failed — route persistence unavailable");
  }

  // --- 12. Event queue ---
  RtosQueueHandle event_queue = RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));

  // --- 13. Construct services ---
  SensorProducer sensor_producer(sensor_manager, event_queue,
                                 {
                                     .task_stack_size = 4096,
                                     .task_priority = 5,
                                 });

  GpsService gps_service(nmea_gps, event_queue,
                         {
                             .baud_rate = GPS_BAUD,
                             .posting_interval_ms = settings.gps_interval_seconds * 1000,
                             .task_stack_size = 4096,
                             .task_priority = 5,
                         });

  InputService input_service(touch, gpio::native::hal, event_queue,
                             {
                                 .pin_cap_int = PIN_CAP_INT,
                                 .pin_button_power = PIN_BUTTON_POWER,
                                 .pin_button_boot = PIN_BUTTON_BOOT,
                             });

  DisplayService display_service({
      .spi_host = SPI_HOST_DISPLAY,
      .pin_cs = PIN_DISPLAY_CS,
      .pin_dc = PIN_DISPLAY_DC,
      .pin_rst = PIN_DISPLAY_RST,
      .pin_busy = PIN_DISPLAY_BUSY,
  });

  PowerService power_service(bms, gpio::native::hal,
                             {
                                 .pin_wake_button_power = PIN_BUTTON_POWER,
                                 .pin_wake_button_boot = PIN_BUTTON_BOOT,
                             });

  UIManager ui_manager({
      .firmware_version = FIRMWARE_VERSION,
      .serial_number = get_serial_number(),
  });

  // --- 14. Init display (shows initial empty dashboard) ---
  DisplayValues initial{};
  display_service.init(initial);

  // --- 15. Start producer tasks ---
  sensor_producer.start();
  gps_service.start();
  input_service.start();

  // --- 16. Construct and run orchestrator ---
  Orchestrator::Services services = {
      .sensor_producer = sensor_producer,
      .gps_service = gps_service,
      .input_service = input_service,
      .display_service = display_service,
      .storage_service = storage,
      .power_service = power_service,
      .ui_manager = ui_manager,
  };

  Orchestrator orchestrator(event_queue, services, settings, config_store);
  orchestrator.init(cause);
  orchestrator.run(); // Never returns.
}

// ===========================================================================
// Helper functions
// ===========================================================================

// ---------------------------------------------------------------------------
// init_nvs
// ---------------------------------------------------------------------------

static void init_nvs() {
  esp_err_t err = nvs_flash_init();
  if (err == ESP_ERR_NVS_NO_FREE_PAGES || err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
    ESP_ERROR_CHECK(nvs_flash_erase());
    err = nvs_flash_init();
  }
  ESP_ERROR_CHECK(err);
}

// ---------------------------------------------------------------------------
// init_i2c_bus
// ---------------------------------------------------------------------------

static i2c_master_bus_handle_t init_i2c_bus() {
  i2c_master_bus_config_t config = {
      .i2c_port = I2C_NUM_0,
      .sda_io_num = PIN_I2C_SDA,
      .scl_io_num = PIN_I2C_SCL,
      .clk_source = I2C_CLK_SRC_DEFAULT,
      .glitch_ignore_cnt = 7,
      .intr_priority = 0,
      .trans_queue_depth = 0,
      .flags =
          {
              .enable_internal_pullup = true,
          },
  };

  i2c_master_bus_handle_t bus = nullptr;
  ESP_ERROR_CHECK(i2c_new_master_bus(&config, &bus));
  AG_LOGI(TAG, "I2C bus ready");
  return bus;
}

// ---------------------------------------------------------------------------
// init_gpio
//
// Configures power-enable pins and sets initial levels.  Exact pins depend
// on the AGo schematic.  Follows the reference product pattern:
// configure as output, set drive capability, set initial levels.
// ---------------------------------------------------------------------------

static void init_gpio() {
  // AGo power-enable pins are TBD from schematic.
  // Add output pin configuration here once hardware routing is finalized.
  // Follow the reference product pattern:
  //
  //   gpio::native::hal.configure(pin, gpio::Mode::Output,
  //                               gpio::PullMode::Floating,
  //                               gpio::InterruptType::Disabled);
  //   gpio_set_drive_capability(pin, GPIO_DRIVE_CAP_3);
  //   gpio::native::hal.set_level(pin, initial_level);
}

// ---------------------------------------------------------------------------
// init_spi_buses
// ---------------------------------------------------------------------------

static void init_spi_buses() {
  // Display SPI bus
  spi_bus_config_t display_bus = {};
  display_bus.mosi_io_num = PIN_DISPLAY_MOSI;
  display_bus.miso_io_num = -1; // display is write-only
  display_bus.sclk_io_num = PIN_DISPLAY_CLK;
  display_bus.quadwp_io_num = -1;
  display_bus.quadhd_io_num = -1;
  display_bus.max_transfer_sz = 4096;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_DISPLAY, &display_bus, SPI_DMA_CH_AUTO));

  // NAND SPI bus (separate host — may be shared with display depending on
  // hardware routing; adjust SPI_HOST_NAND in board_config.h if sharing)
  spi_bus_config_t nand_bus = {};
  nand_bus.mosi_io_num = PIN_NAND_MOSI;
  nand_bus.miso_io_num = PIN_NAND_MISO;
  nand_bus.sclk_io_num = PIN_NAND_CLK;
  nand_bus.quadwp_io_num = -1;
  nand_bus.quadhd_io_num = -1;
  nand_bus.max_transfer_sz = 4096;
  ESP_ERROR_CHECK(spi_bus_initialize(SPI_HOST_NAND, &nand_bus, SPI_DMA_CH_AUTO));

  AG_LOGI(TAG, "SPI buses ready");
}

// ---------------------------------------------------------------------------
// get_serial_number
//
// Returns a 12-character hex string derived from the ESP32 base MAC address.
// The returned pointer is valid for the lifetime of the program (static buf).
// ---------------------------------------------------------------------------

static const char *get_serial_number() {
  static char serial[13] = {};
  if (serial[0] == '\0') {
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    snprintf(serial, sizeof(serial), "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
  }
  return serial;
}

// ---------------------------------------------------------------------------
// measures_to_ago
//
// Extract AGo-relevant fields from the full Measures struct returned by
// SensorManager::start_measures().
// ---------------------------------------------------------------------------

static MeasuresAGo measures_to_ago(const Measures &m) {
  MeasuresAGo ago{};
  ago.temp_hum_a = m.temp_hum_a;
  ago.pm_a = m.pm_a;
  ago.co2 = m.co2;
  ago.tvoc_nox = m.tvoc_nox;
  ago.power = m.power;
  return ago;
}

// ---------------------------------------------------------------------------
// build_fast_path_display
//
// Build a minimal DisplayValues for the locked dashboard without a
// UIManager.  No chart data, no menu state, no snackbar.
// ---------------------------------------------------------------------------

static DisplayValues build_fast_path_display(const Measures &measures, const GpsData &gps,
                                             const PowerSnapshot &bms, const GoSettings &settings) {
  DisplayValues v{};

  // Sensor readings
  if (measures.co2.is_valid()) {
    v.co2_ppm = measures.co2.co2;
  }
  if (measures.pm_a.is_pm_25_valid()) {
    v.pm25_ugm3 = measures.pm_a.pm_25;
  }
  if (measures.temp_hum_a.is_temp_valid()) {
    v.temperature_c = measures.temp_hum_a.temperature;
  }
  if (measures.temp_hum_a.is_hum_valid()) {
    v.humidity_pct = measures.temp_hum_a.humidity;
  }
  if (measures.tvoc_nox.is_tvoc_index_valid()) {
    v.tvoc_index = measures.tvoc_nox.tvoc_index;
  }
  if (measures.tvoc_nox.is_nox_index_valid()) {
    v.nox_index = measures.tvoc_nox.nox_index;
  }

  // GPS clock
  if (is_gps_timestamp_valid(gps.timestamp)) {
    v.hour = static_cast<uint8_t>(gps.timestamp.hour);
    v.minute = static_cast<uint8_t>(gps.timestamp.minute);
  }

  // GPS status
  v.gps_fix = is_fix_valid(gps.fix);

  // Battery
  if (bms.battery_percentage >= 0.0f) {
    v.battery_pct = static_cast<uint8_t>(bms.battery_percentage);
  }
  v.is_battery_charging = (bms.charging_status != BmsChargingState::Unknown &&
                           bms.charging_status != BmsChargingState::NotCharging);

  // State
  v.locked = true;
  v.screen = Screen::Home;

  // Settings-derived
  v.use_fahrenheit = settings.use_fahrenheit;
  v.pm_use_usaqi = settings.pm_use_usaqi;
  v.display_off = (settings.display_refresh_interval_seconds == 0);

  return v;
}

// ---------------------------------------------------------------------------
// compute_fast_path_sleep_duration
//
// Same logic as Orchestrator::compute_sleep_duration_ms() — returns the
// minimum of measurement interval and display refresh interval.
// ---------------------------------------------------------------------------

static uint32_t compute_fast_path_sleep_duration(const GoSettings &settings) {
  uint32_t duration_ms = static_cast<uint32_t>(settings.measurement_interval_seconds) * 1000;

  if (settings.display_refresh_interval_seconds > 0) {
    uint32_t display_ms = static_cast<uint32_t>(settings.display_refresh_interval_seconds) * 1000;
    duration_ms = std::min(duration_ms, display_ms);
  }

  return duration_ms;
}
