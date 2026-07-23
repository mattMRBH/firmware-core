/**
 * AirGradient Go -- local-server product service
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_LOCAL_SERVER_H
#define GO_LOCAL_SERVER_H

#include <cstddef>
#include <cstdint>
#include <optional>
#include <type_traits>

#include "go_config_types.h"
#include "go_settings.h"
#include "hal/action_handler.h"
#include "hal/config_provider.h"
#include "hal/measures_provider.h"
#include "measures_types.h"
#include "rtos.h"
#include "types/local_config.h"
#include "types/local_server_result.h"
#include "types/system_info.h"

enum class LocalApiRequestKind : uint8_t {
  Config,
  Action,
};

struct LocalApiRequest {
  LocalApiRequestKind kind = LocalApiRequestKind::Config;
  GoConfigUpdate config{};
  ActionId action = ActionId::CalibrateCo2;
};

static_assert(std::is_trivially_copyable<LocalApiRequest>::value,
              "Local API requests must be queue-copyable");

static constexpr size_t LOCAL_API_REQUEST_QUEUE_DEPTH = 4;

inline constexpr const char *STATIONARY_AGO_MODEL_CODE = "P-1PSG";

class GoLocalServerService final : public MeasuresProvider,
                                   public ConfigProvider,
                                   public ActionHandler {
public:
  struct Config {
    const char *serial_number = nullptr;
    const char *firmware_version = nullptr;
    const char *model = STATIONARY_AGO_MODEL_CODE;
    bool co2_calibration_supported = false;
  };

  GoLocalServerService(RtosQueueHandle event_queue, const Config &config);

  bool is_valid() const;

  Measures get_measures() override;
  SystemInfo get_system_info() override;
  LocalServerConfig get_config() override;
  ConfigSubmitResult submit_config(const LocalServerConfig &partial) override;
  ActionResult trigger(ActionId action) override;

  void publish_measurement_snapshot(const MeasuresAGo &corrected, uint32_t boot_count);
  void publish_config_snapshot(const GoSettings &settings);
  void publish_wifi_rssi(std::optional<int> wifi_rssi);

  void set_access(ConfigAccess access);
  ConfigAccess access() const;

  void set_co2_calibration_supported(bool supported);
  bool release_co2_calibration();

  bool pop_request(uint32_t event_epoch, LocalApiRequest &request);
  size_t clear_requests();
  uint32_t queue_epoch() const;

private:
  friend class GoLocalServerServiceTestAccess;

  enum class CalibrationState : uint8_t {
    Idle,
    Queued,
    Active,
  };

  struct ActiveConfigSnapshot {
    bool pm_use_usaqi = false;
    bool use_fahrenheit = false;
    bool disable_cloud = false;
    ConfigurationControl configuration_control = ConfigurationControl::Both;
    MeasurementCorrections corrections{};
  };

  static Measures map_measures(const MeasuresAGo &corrected);
  static ActiveConfigSnapshot make_active_config(const GoSettings &settings);
  static LocalServerConfig map_config(const ActiveConfigSnapshot &active);
  static ConfigFieldId first_unsupported_field(const LocalServerConfig &partial);
  static bool is_exact_control_recovery(const LocalServerConfig &partial);
  static ConfigSubmitResult translate_config(const LocalServerConfig &partial,
                                             const ActiveConfigSnapshot &active,
                                             GoConfigUpdate &update);
  static bool translate_pm25_correction(const CorrectionEntry &entry, Pm25Correction &correction);
  static bool translate_linear_correction(const CorrectionEntry &entry,
                                          LinearCorrection &correction);

  ConfigSubmitResult admit_config(const GoConfigUpdate &update, bool exact_control_recovery,
                                  uint32_t expected_epoch);
  bool append_and_signal_locked(const LocalApiRequest &request);
  void rollback_tail_locked();
  bool lock() const;

  RtosQueueHandle _event_queue;
  mutable RtosMutex _mutex;

  Measures _measures{};
  SystemInfo _system_info{};
  LocalServerConfig _config{};
  ActiveConfigSnapshot _active_config{};

  ConfigAccess _access = ConfigAccess::Disabled;
  bool _co2_calibration_supported = false;
  CalibrationState _calibration_state = CalibrationState::Idle;

  LocalApiRequest _requests[LOCAL_API_REQUEST_QUEUE_DEPTH]{};
  size_t _head = 0;
  size_t _tail = 0;
  size_t _count = 0;
  uint32_t _queue_epoch = 0;
};

#endif // GO_LOCAL_SERVER_H
