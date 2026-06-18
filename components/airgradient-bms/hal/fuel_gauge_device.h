/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef FUEL_GAUGE_DEVICE_H
#define FUEL_GAUGE_DEVICE_H

#include <cstddef>
#include <cstdint>

/// Abstract fuel gauge device interface for runtime polling.
///
/// Parallel to BmsDevice.  Only the runtime-poll API needs polymorphism;
/// boot-time Data Memory access stays on the concrete BQ27427 class
/// (only GoHardwareBoard::init_bms calls those, no test surface needed).
///
/// PowerService stores a FuelGaugeDevice pointer.  Tests substitute a
/// mock subclass with scripted return values, the same way they
/// substitute BmsDevice.
class FuelGaugeDevice {
public:
  virtual ~FuelGaugeDevice() = default;

  /// True when the chip has been successfully attached and identified.
  virtual bool ready() const = 0;

  // -- Runtime poll surface (used by PowerService::poll_bms) ---------------

  virtual bool read_soc_percent(uint8_t &out) = 0;
  virtual bool read_voltage_mv(uint16_t &out) = 0;
  virtual bool read_average_current_ma(int16_t &out) = 0;
  virtual bool read_average_power_mw(int16_t &out) = 0;
  virtual bool read_remaining_capacity_mah(uint16_t &out) = 0;
  virtual bool read_full_charge_capacity_mah(uint16_t &out) = 0;
  virtual bool read_internal_temperature_c(float &out) = 0;
  virtual bool read_flags(uint16_t &out) = 0;

  // -- Fuel-gauge learning reads (used by PowerService verify/poll) --------
  //
  // Non-perturbing reads needed by the factory learning path. Kept on the
  // HAL (not just the concrete driver) so PowerService can aggregate them
  // through its FuelGaugeDevice pointer and host tests can mock them.

  /// CONTROL_STATUS register (Control(0x0000)) for QMAX_UP / RES_UP.
  virtual bool read_control_status(uint16_t &out) = 0;

  /// Learned Qmax for cell 0 (State subclass bytes 0/1), raw gauge units.
  virtual bool read_qmax_cell0(uint16_t &out) = 0;

  /// Ra impedance grid (Ra0 RAM subclass). @p len must be >= FG_RA_TABLE_SIZE.
  virtual bool read_ra_table(int16_t *out, size_t len) = 0;

  /// Configured Design Capacity (Data Memory read; non-perturbing).
  virtual bool read_design_capacity_mah(uint16_t &out) = 0;
};

#endif // FUEL_GAUGE_DEVICE_H
