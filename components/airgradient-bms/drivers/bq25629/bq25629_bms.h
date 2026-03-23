/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef BQ25629_BMS_H
#define BQ25629_BMS_H

#include <cstdint>

#include "bq25629.h"
#include "driver/i2c_master.h"

#include "hal/bms_device.h"

/// BmsDevice adapter for the TI BQ25629 single-cell charger IC.
///
/// Wraps the vendor-level @c drivers::BQ25629 component and maps it to
/// the @c BmsDevice HAL interface used by the BMS stack.
///
/// The adapter owns the vendor driver instance internally.  Product code
/// supplies the I2C bus handle and a @c drivers::BQ25629_Config at
/// construction time; @c init() forwards the config to the vendor driver.
class BQ25629Bms : public BmsDevice {
public:
  /// Construct the adapter.
  /// @param i2c_bus  I2C master bus handle (shared, not owned).
  /// @param config   Charger configuration forwarded to the vendor driver.
  /// @param address  I2C device address (default 0x6A).
  explicit BQ25629Bms(i2c_master_bus_handle_t i2c_bus, const drivers::BQ25629_Config &config,
                      uint8_t address = 0x6A);

  ~BQ25629Bms() override = default;

  // -- BmsDevice interface --------------------------------------------------

  bool init() override;
  bool read_telemetry(BmsTelemetry &out) override;
  bool read_status(BmsStatus &out) override;
  bool get_battery_percentage(float *output) override;
  bool update_watchdog() override;
  bool feature_ship_available() const override;
  bool enter_ship_mode() override;

private:
  drivers::BQ25629 _charger;
  drivers::BQ25629_Config _config;

  /// Minimum interval between watchdog resets (ms).
  static constexpr uint32_t WATCHDOG_UPDATE_INTERVAL_MS = 10000;

  /// Last time the watchdog was reset (ms, monotonic).
  uint64_t _last_watchdog_reset_ms;
};

#endif // BQ25629_BMS_H
