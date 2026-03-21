/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef SIMCOM_A7672X_H
#define SIMCOM_A7672X_H

#if __has_include("airgradient_gpio.h")
#include "airgradient_gpio.h"
#else
#include "../../../airgradient-gpio/hal/airgradient_gpio.h"
#endif

#include "../../hal/cellular_modem.h"
#include "../../services/at_command_handler.h"

class TestableSimcomA7672x;

class SimcomA7672x : public CellularModem {
  friend class TestableSimcomA7672x;

public:
  static constexpr uint32_t POWER_ON_LOW_DELAY_MS = 500;
  static constexpr uint32_t POWER_ON_HIGH_DELAY_MS = 100;
  static constexpr uint32_t FORCE_POWER_OFF_HIGH_DELAY_MS = 1300;
  static constexpr uint32_t RESET_RECOVERY_DELAY_MS = 2000;
  static constexpr uint32_t REINITIALIZE_STEP_DELAY_MS = 2000;
  static constexpr uint32_t REGISTRATION_RETRY_DELAY_MS = 1000;
  static constexpr uint32_t REGISTRATION_POLL_DELAY_MS = 3000;
  static constexpr uint32_t REGISTRATION_LOOP_DELAY_MS = 10;
  static constexpr uint32_t REGISTRATION_CHECK_TIMEOUT_MS = 60000;
  static constexpr uint32_t SERVICE_STATUS_TIMEOUT_MS = 30000;
  static constexpr int MIN_VALID_SIGNAL_CSQ = 1;
  static constexpr int MAX_VALID_SIGNAL_CSQ = 31;
  static constexpr int MIN_READY_SIGNAL_CSQ = 10;
  static constexpr uint32_t DEFAULT_OPERATOR_SCAN_TIMEOUT_MS = 600000;
  static constexpr uint32_t OPERATOR_SELECTION_TIMEOUT_MS = 60000;
  static constexpr uint32_t DNS_RESOLUTION_TIMEOUT_MS = 10000;
  static constexpr int32_t DEFAULT_HTTP_CONNECTION_TIMEOUT_SECONDS = 120;
  static constexpr int32_t DEFAULT_HTTP_RESPONSE_TIMEOUT_SECONDS = 20;
  static constexpr int32_t MIN_HTTP_CONNECTION_TIMEOUT_SECONDS = 20;
  static constexpr int32_t MAX_HTTP_CONNECTION_TIMEOUT_SECONDS = 120;
  static constexpr int32_t MIN_HTTP_RESPONSE_TIMEOUT_SECONDS = 2;
  static constexpr int32_t MAX_HTTP_RESPONSE_TIMEOUT_SECONDS = 120;
  static constexpr size_t HTTP_READ_CHUNK_SIZE = 200;
  static constexpr uint32_t HTTP_INIT_TIMEOUT_MS = 20000;
  static constexpr uint32_t HTTP_INIT_RETRY_DELAY_MS = 2000;
  static constexpr uint32_t HTTP_DATA_UPLOAD_TIMEOUT_MS = 10000;
  static constexpr uint32_t HTTP_GET_RETRY_DELAY_MS = 2000;
  static constexpr int HTTP_GET_MAX_RETRIES = 3;
  static constexpr int HTTP_MODULE_ERROR_CODE_MIN = 700;
  static constexpr int HTTP_MODULE_ERROR_CODE_MAX = 720;

  explicit SimcomA7672x(AirgradientSerial &serial, uint32_t warm_up_time_ms = 0);

  SimcomA7672x(AirgradientSerial &serial, const gpio::Hal &power_hal, int power_pin,
               uint32_t warm_up_time_ms = 0);

  CellularStatus init() override;
  CellularStatus power_on() override;
  CellularStatus power_off(bool force) override;
  CellularStatus reset() override;
  CellularStatus sleep() override;
  CellularStatus reinitialize() override;

  CellularStatus get_module_info(MutableStringBuffer out_module_info,
                                 size_t *out_length = nullptr) override;
  CellularStatus get_sim_iccid(MutableStringBuffer out_iccid,
                               size_t *out_length = nullptr) override;
  CellularStatus check_sim_ready() override;
  CellularStatus get_signal_quality(CellularSignalQuality *out_signal_quality) override;
  CellularStatus get_ip_address(MutableStringBuffer out_ip_address,
                                size_t *out_length = nullptr) override;

  CellularStatus get_network_registration(CellularTechnology technology,
                                          CellularNetworkRegistration *out_registration) override;
  CellularStatus start_network_registration(const CellularRegistrationRequest &request) override;

  CellularStatus resolve_dns(const char *hostname, MutableStringBuffer out_ip_address,
                             size_t *out_length = nullptr) override;

  CellularStatus http_get(const CellularHttpGetRequest &request,
                          CellularHttpResponse *out_response) override;
  CellularStatus http_post(const CellularHttpPostRequest &request,
                           CellularHttpResponse *out_response) override;

private:
  enum class NetworkRegistrationState : uint8_t {
    CheckModuleReady,
    PrepareModule,
    ScanOperators,
    ConfigureManualNetwork,
    CheckNetworkRegistration,
    CheckServiceStatus,
    NetworkReady,
    OperatorListExhausted,
  };

  CellularStatus _map_at_response_to_status(ATCommandHandler::Response response) const;
  int _map_cellular_technology_to_mode(CellularTechnology technology) const;
  const char *_map_cellular_technology_to_registration_command(CellularTechnology technology) const;
  CellularRegistrationState _map_registration_state(int stat) const;
  CellularStatus
  _send_at_expect_ok(const char *command,
                     uint32_t timeout_ms = ATCommandHandler::DEFAULT_WAIT_RESPONSE_TIMEOUT_MS);
  CellularStatus
  _send_raw_expect_ok(const char *command,
                      uint32_t timeout_ms = ATCommandHandler::DEFAULT_WAIT_RESPONSE_TIMEOUT_MS);
  CellularStatus _disable_network_registration_urc(CellularTechnology technology);
  CellularStatus _apply_cellular_technology(CellularTechnology technology);
  CellularStatus _apply_apn(const char *apn);
  CellularStatus _is_service_available(bool *out_service_available);
  CellularStatus _activate_pdp_context();
  CellularStatus _ensure_packet_domain_attached(bool force_attach, bool *out_is_attached);
  CellularStatus
  _parse_network_registration_line(const char *line,
                                   CellularNetworkRegistration *out_registration) const;
  CellularStatus _scan_available_operators(OperatorList *out_operators, uint32_t timeout_ms,
                                           MutableStringBuffer work_buffer);
  CellularStatus _parse_operator_list(const char *response, OperatorList *out_operators) const;
  CellularStatus _apply_operator_selection(uint32_t operator_id, OperatorAccessTech access_tech);
  CellularStatus _http_init();
  CellularStatus _http_terminate();
  CellularStatus _http_set_timeouts(int32_t connection_timeout_seconds,
                                    int32_t response_timeout_seconds);
  CellularStatus _http_set_url(const char *url);
  CellularStatus _http_set_content_type(const char *content_type);
  CellularStatus _http_action(int method_code, int32_t connection_timeout_seconds,
                              int32_t response_timeout_seconds, uint16_t *out_status_code,
                              size_t *out_body_size);
  CellularStatus _http_read_body(MutableBuffer response_buffer, size_t body_size,
                                 size_t *out_bytes_written, bool *out_is_truncated);
  int32_t _resolve_timeout(int32_t user_timeout, int32_t default_timeout, int32_t min_timeout,
                           int32_t max_timeout) const;
  uint32_t _calculate_http_action_timeout_ms(int32_t connection_timeout_seconds,
                                             int32_t response_timeout_seconds) const;
  NetworkRegistrationState _impl_check_module_ready();
  NetworkRegistrationState _impl_prepare_module(CellularTechnology technology, const char *apn);
  NetworkRegistrationState _impl_scan_operators(uint32_t scan_timeout_ms,
                                                MutableStringBuffer work_buffer);
  NetworkRegistrationState
  _impl_configure_manual_network(uint64_t *out_registration_check_start_ms);
  NetworkRegistrationState _impl_check_network_registration(CellularTechnology technology,
                                                            uint64_t registration_check_start_ms,
                                                            uint64_t *out_service_status_start_ms);
  NetworkRegistrationState _impl_check_service_status(uint64_t service_status_start_ms);
  CellularStatus _impl_network_ready();
  void _drain_terminal_response();
  int _csq_to_rssi_dbm(int csq) const;
  bool _has_power_control() const;

  ATCommandHandler _at_command_handler;
  const gpio::Hal *_hal_gpio = nullptr;
  int _power_pin = gpio::INVALID_PIN;
  uint32_t _warm_up_time_ms = 0;
  bool _is_initialized = false;
  OperatorList _scanned_operators = {};
  size_t _current_operator_index = 0;
  OperatorInfo _last_successful_operator = {};
};

#endif // SIMCOM_A7672X_H
