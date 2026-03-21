/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "simcom_a7672x.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#if __has_include("rtos.h")
#include "rtos.h"
#else
#include "../../../airgradient-common/include/rtos.h"
#endif

SimcomA7672x::SimcomA7672x(AirgradientSerial &serial, uint32_t warm_up_time_ms)
    : _at_command_handler(serial), _warm_up_time_ms(warm_up_time_ms) {}

SimcomA7672x::SimcomA7672x(AirgradientSerial &serial, const gpio::Hal &power_hal, int power_pin,
                           uint32_t warm_up_time_ms)
    : _at_command_handler(serial), _hal_gpio(&power_hal), _power_pin(power_pin),
      _warm_up_time_ms(warm_up_time_ms) {}

CellularStatus SimcomA7672x::init() {
  if (_is_initialized) {
    return CellularStatus::Ok;
  }

  if (_has_power_control()) {
    if (!_hal_gpio->configure(_power_pin, gpio::Mode::Output, gpio::PullMode::Floating,
                              gpio::InterruptType::Disabled)) {
      return CellularStatus::Failed;
    }

    const CellularStatus power_status = power_on();
    if (power_status != CellularStatus::Ok) {
      return power_status;
    }
  }

  return reinitialize();
}

CellularStatus SimcomA7672x::power_on() {
  if (!_has_power_control()) {
    return CellularStatus::Unsupported;
  }

  if (!_hal_gpio->set_level(_power_pin, 0)) {
    return CellularStatus::Failed;
  }
  RTOS::delay_ms(POWER_ON_LOW_DELAY_MS);

  if (!_hal_gpio->set_level(_power_pin, 1)) {
    return CellularStatus::Failed;
  }
  RTOS::delay_ms(POWER_ON_HIGH_DELAY_MS);

  if (!_hal_gpio->set_level(_power_pin, 0)) {
    return CellularStatus::Failed;
  }
  RTOS::delay_ms(POWER_ON_HIGH_DELAY_MS);

  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::power_off(bool force) {
  if (!_has_power_control()) {
    return CellularStatus::Unsupported;
  }
  if (!force) {
    return CellularStatus::Unsupported;
  }

  if (!_hal_gpio->set_level(_power_pin, 1)) {
    return CellularStatus::Failed;
  }
  RTOS::delay_ms(FORCE_POWER_OFF_HIGH_DELAY_MS);

  if (!_hal_gpio->set_level(_power_pin, 0)) {
    return CellularStatus::Failed;
  }

  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::reset() {
  if (!_has_power_control()) {
    return CellularStatus::Unsupported;
  }

  const CellularStatus power_off_status = power_off(true);
  if (power_off_status != CellularStatus::Ok) {
    return power_off_status;
  }

  RTOS::delay_ms(RESET_RECOVERY_DELAY_MS);
  return power_on();
}

CellularStatus SimcomA7672x::sleep() { return CellularStatus::Unsupported; }

CellularStatus SimcomA7672x::reinitialize() {
  if (!_at_command_handler.test_at()) {
    return CellularStatus::Error;
  }

  CellularStatus status = _send_at_expect_ok("E0");
  if (status != CellularStatus::Ok) {
    return status;
  }
  RTOS::delay_ms(REINITIALIZE_STEP_DELAY_MS);

  status = _send_at_expect_ok("+CGEREP=0");
  if (status != CellularStatus::Ok) {
    return status;
  }
  RTOS::delay_ms(REINITIALIZE_STEP_DELAY_MS);

  status = _send_raw_expect_ok("ATI");
  if (status != CellularStatus::Ok) {
    return status;
  }

  _is_initialized = true;
  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::get_module_info(MutableStringBuffer out_module_info,
                                             size_t *out_length) {
  _at_command_handler.send_raw("ATI");
  _drain_terminal_response();
  (void)out_module_info;
  (void)out_length;
  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::get_sim_iccid(MutableStringBuffer out_iccid, size_t *out_length) {
  _at_command_handler.send_at("+CICCID");
  if (_at_command_handler.wait_response(ATCommandHandler::DEFAULT_WAIT_RESPONSE_TIMEOUT_MS,
                                        "+ICCID:") != ATCommandHandler::Response::Expected1) {
    return CellularStatus::Timeout;
  }

  const CellularStatus line_status = _at_command_handler.wait_and_read_line(out_iccid, out_length);
  if (line_status != CellularStatus::Ok) {
    return line_status;
  }

  _drain_terminal_response();
  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::check_sim_ready() {
  char state[32] = {};
  size_t state_length = 0;

  _at_command_handler.send_at("+CPIN?");
  if (_at_command_handler.wait_response(ATCommandHandler::DEFAULT_WAIT_RESPONSE_TIMEOUT_MS,
                                        "+CPIN:") != ATCommandHandler::Response::Expected1) {
    return CellularStatus::Timeout;
  }

  const CellularStatus line_status =
      _at_command_handler.wait_and_read_line({state, sizeof(state)}, &state_length);
  if (line_status != CellularStatus::Ok) {
    return line_status;
  }

  if (::strcmp(state, "READY") != 0) {
    _drain_terminal_response();
    return CellularStatus::Failed;
  }

  _drain_terminal_response();
  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::get_signal_quality(CellularSignalQuality *out_signal_quality) {
  if (out_signal_quality == nullptr) {
    return CellularStatus::InvalidArgument;
  }

  char response_line[32] = {};
  size_t line_length = 0;

  _at_command_handler.send_at("+CSQ");
  if (_at_command_handler.wait_response(ATCommandHandler::DEFAULT_WAIT_RESPONSE_TIMEOUT_MS,
                                        "+CSQ:") != ATCommandHandler::Response::Expected1) {
    return CellularStatus::Timeout;
  }

  const CellularStatus line_status =
      _at_command_handler.wait_and_read_line({response_line, sizeof(response_line)}, &line_length);
  if (line_status != CellularStatus::Ok) {
    return line_status;
  }

  char *separator = ::strchr(response_line, ',');
  if (separator == nullptr) {
    return CellularStatus::Failed;
  }

  *separator = '\0';
  char *parse_end = nullptr;
  const long csq = ::strtol(response_line, &parse_end, 10);
  if (parse_end == response_line || *parse_end != '\0') {
    return CellularStatus::Failed;
  }

  out_signal_quality->csq = static_cast<int>(csq);
  out_signal_quality->rssi_dbm = _csq_to_rssi_dbm(out_signal_quality->csq);

  _drain_terminal_response();
  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::get_ip_address(MutableStringBuffer out_ip_address,
                                            size_t *out_length) {
  _at_command_handler.send_at("+CGPADDR=1");
  if (_at_command_handler.wait_response(ATCommandHandler::DEFAULT_WAIT_RESPONSE_TIMEOUT_MS,
                                        "+CGPADDR: 1,") != ATCommandHandler::Response::Expected1) {
    return CellularStatus::Timeout;
  }

  const CellularStatus line_status =
      _at_command_handler.wait_and_read_line(out_ip_address, out_length);
  if (line_status != CellularStatus::Ok) {
    return line_status;
  }

  _drain_terminal_response();
  return CellularStatus::Ok;
}

CellularStatus
SimcomA7672x::get_network_registration(CellularTechnology technology,
                                       CellularNetworkRegistration *out_registration) {
  if (out_registration == nullptr) {
    return CellularStatus::InvalidArgument;
  }

  const char *registration_command = _map_cellular_technology_to_registration_command(technology);
  if (registration_command == nullptr) {
    return CellularStatus::Unsupported;
  }

  char command[16] = {};
  char prefix[16] = {};
  snprintf(command, sizeof(command), "+%s?", registration_command);
  snprintf(prefix, sizeof(prefix), "+%s:", registration_command);

  _at_command_handler.send_at(command);
  const ATCommandHandler::Response response =
      _at_command_handler.wait_response(ATCommandHandler::DEFAULT_WAIT_RESPONSE_TIMEOUT_MS, prefix);
  if (response != ATCommandHandler::Response::Expected1) {
    return _map_at_response_to_status(response);
  }

  char registration_line[64] = {};
  const CellularStatus line_status =
      _at_command_handler.wait_and_read_line({registration_line, sizeof(registration_line)});
  if (line_status != CellularStatus::Ok) {
    return line_status;
  }

  const CellularStatus parse_status =
      _parse_network_registration_line(registration_line, out_registration);
  _drain_terminal_response();
  return parse_status;
}

CellularStatus
SimcomA7672x::start_network_registration(const CellularRegistrationRequest &request) {
  if (request.apn == nullptr || request.apn[0] == '\0') {
    return CellularStatus::InvalidArgument;
  }

  NetworkRegistrationState state = NetworkRegistrationState::CheckModuleReady;
  uint64_t operation_start_ms = RTOS::get_time_ms();
  uint64_t service_status_start_ms = 0;
  uint64_t registration_check_start_ms = 0;
  char operator_scan_buffer[512] = {};

  while ((RTOS::get_time_ms() - operation_start_ms) < request.operation_timeout_ms) {
    switch (state) {
    case NetworkRegistrationState::CheckModuleReady:
      state = _impl_check_module_ready();
      break;

    case NetworkRegistrationState::PrepareModule:
      state = _impl_prepare_module(request.technology, request.apn);
      break;

    case NetworkRegistrationState::ScanOperators:
      state = _impl_scan_operators(request.scan_timeout_ms,
                                   {operator_scan_buffer, sizeof(operator_scan_buffer)});
      break;

    case NetworkRegistrationState::ConfigureManualNetwork:
      state = _impl_configure_manual_network(&registration_check_start_ms);
      break;

    case NetworkRegistrationState::CheckNetworkRegistration:
      state = _impl_check_network_registration(request.technology, registration_check_start_ms,
                                               &service_status_start_ms);
      break;

    case NetworkRegistrationState::CheckServiceStatus:
      state = _impl_check_service_status(service_status_start_ms);
      break;

    case NetworkRegistrationState::NetworkReady: {
      CellularStatus status = _impl_network_ready();
      if (status == CellularStatus::Ok) {
        return CellularStatus::Ok;
      }
      if (status == CellularStatus::Timeout) {
        state = NetworkRegistrationState::CheckModuleReady;
      } else {
        state = NetworkRegistrationState::CheckServiceStatus;
      }
      break;
    }

    case NetworkRegistrationState::OperatorListExhausted:
      _current_operator_index = 0;
      _scanned_operators.count = 0;
      RTOS::delay_ms(REGISTRATION_RETRY_DELAY_MS);
      state = NetworkRegistrationState::ScanOperators;
      break;
    }

    RTOS::delay_ms(REGISTRATION_LOOP_DELAY_MS);
  }

  return CellularStatus::Timeout;
}

CellularStatus SimcomA7672x::resolve_dns(const char *hostname, MutableStringBuffer out_ip_address,
                                         size_t *out_length) {
  // Validate inputs
  if (hostname == nullptr || hostname[0] == '\0' || out_ip_address.data == nullptr ||
      out_ip_address.size == 0) {
    return CellularStatus::InvalidArgument;
  }

  // Build AT+CDNSGIP command
  char command[128] = {};
  snprintf(command, sizeof(command), "+CDNSGIP=\"%s\"", hostname);

  _at_command_handler.send_at(command);

  // Wait for +CDNSGIP: response
  const ATCommandHandler::Response response =
      _at_command_handler.wait_response(DNS_RESOLUTION_TIMEOUT_MS, "+CDNSGIP:");
  if (response != ATCommandHandler::Response::Expected1) {
    return _map_at_response_to_status(response);
  }

  // Read the response line
  char response_line[128] = {};
  const CellularStatus line_status =
      _at_command_handler.wait_and_read_line({response_line, sizeof(response_line)}, out_length);
  if (line_status != CellularStatus::Ok) {
    return line_status;
  }

  // Parse response: format is "1,\"hostname\",\"ip_address\""
  // Find the last two quote marks
  const char *last_quote = ::strrchr(response_line, '"');
  if (last_quote == nullptr || last_quote == response_line) {
    return CellularStatus::Failed;
  }

  // Find second-to-last quote by searching backwards from last_quote - 1
  const char *second_last_quote = nullptr;
  for (const char *p = last_quote - 1; p >= response_line; p--) {
    if (*p == '"') {
      second_last_quote = p;
      break;
    }
  }

  if (second_last_quote == nullptr) {
    return CellularStatus::Failed;
  }

  // Extract IP address
  const size_t ip_length = last_quote - second_last_quote - 1;
  if (ip_length == 0 || ip_length >= out_ip_address.size) {
    return CellularStatus::BufferTooSmall;
  }

  ::strncpy(out_ip_address.data, second_last_quote + 1, ip_length);
  out_ip_address.data[ip_length] = '\0';

  if (out_length != nullptr) {
    *out_length = ip_length;
  }

  _drain_terminal_response();
  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::http_get(const CellularHttpGetRequest &request,
                                      CellularHttpResponse *out_response) {
  // Input validation
  if (request.url == nullptr || request.url[0] == '\0' || out_response == nullptr) {
    return CellularStatus::InvalidArgument;
  }

  // Initialize response
  out_response->status_code = CELLULAR_HTTP_STATUS_INVALID;
  out_response->body_size = 0;
  out_response->body_bytes_written = 0;
  out_response->is_body_truncated = false;

  // HTTP Init
  CellularStatus status = _http_init();
  if (status != CellularStatus::Ok) {
    return status;
  }

  // Set timeouts
  status = _http_set_timeouts(request.timeouts.connection_timeout_seconds,
                              request.timeouts.response_timeout_seconds);
  if (status != CellularStatus::Ok) {
    _http_terminate();
    return status;
  }

  // Set URL
  status = _http_set_url(request.url);
  if (status != CellularStatus::Ok) {
    _http_terminate();
    return status;
  }

  // Execute GET with retry logic (up to 3 attempts)
  uint16_t status_code = 0;
  size_t body_size = 0;
  int retry_count = 0;

  while (retry_count < HTTP_GET_MAX_RETRIES) {
    status = _http_action(0, request.timeouts.connection_timeout_seconds,
                          request.timeouts.response_timeout_seconds, &status_code, &body_size);

    if (status == CellularStatus::Ok) {
      break; // Success
    }

    // Only retry on Failed (module errors), not Timeout or Error
    if (status != CellularStatus::Failed) {
      _http_terminate();
      return status;
    }

    retry_count++;
    if (retry_count < HTTP_GET_MAX_RETRIES) {
      RTOS::delay_ms(HTTP_GET_RETRY_DELAY_MS);
    }
  }

  if (status != CellularStatus::Ok) {
    _http_terminate();
    return CellularStatus::Failed; // All retries exhausted
  }

  // Set status code and body size
  out_response->status_code = status_code;
  out_response->body_size = body_size;

  // Read response body if buffer provided and body exists
  if (body_size > 0 && request.response_body.data != nullptr && request.response_body.size > 0) {
    size_t bytes_written = 0;
    bool is_truncated = false;

    status = _http_read_body(request.response_body, body_size, &bytes_written, &is_truncated);
    if (status != CellularStatus::Ok) {
      _http_terminate();
      return status;
    }

    out_response->body_bytes_written = bytes_written;
    out_response->is_body_truncated = is_truncated;
  }

  // Terminate HTTP
  _http_terminate();

  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::http_post(const CellularHttpPostRequest &request,
                                       CellularHttpResponse *out_response) {
  // Input validation
  if (request.url == nullptr || request.url[0] == '\0' || request.body == nullptr ||
      out_response == nullptr) {
    return CellularStatus::InvalidArgument;
  }

  // Initialize response
  out_response->status_code = CELLULAR_HTTP_STATUS_INVALID;
  out_response->body_size = 0;
  out_response->body_bytes_written = 0;
  out_response->is_body_truncated = false;

  // HTTP Init
  CellularStatus status = _http_init();
  if (status != CellularStatus::Ok) {
    return status;
  }

  // Set timeouts
  status = _http_set_timeouts(request.timeouts.connection_timeout_seconds,
                              request.timeouts.response_timeout_seconds);
  if (status != CellularStatus::Ok) {
    _http_terminate();
    return status;
  }

  // Set content-type if provided
  if (request.content_type != nullptr && request.content_type[0] != '\0') {
    status = _http_set_content_type(request.content_type);
    if (status != CellularStatus::Ok) {
      _http_terminate();
      return status;
    }
  }

  // Set URL
  status = _http_set_url(request.url);
  if (status != CellularStatus::Ok) {
    _http_terminate();
    return status;
  }

  // Upload body data
  const size_t body_length = ::strlen(request.body);
  char command[64] = {};
  ::snprintf(command, sizeof(command), "+HTTPDATA=%zu,10", body_length);
  _at_command_handler.send_at(command);

  // Wait for DOWNLOAD prompt
  ATCommandHandler::Response response = _at_command_handler.wait_response("DOWNLOAD");
  if (response != ATCommandHandler::Response::Expected1) {
    _http_terminate();
    return CellularStatus::Error;
  }

  // Send raw body data
  _at_command_handler.send_raw(request.body);

  // Wait for OK (10 second timeout)
  response = _at_command_handler.wait_response(HTTP_DATA_UPLOAD_TIMEOUT_MS);
  if (response != ATCommandHandler::Response::Expected1) {
    _http_terminate();
    return _map_at_response_to_status(response);
  }

  // Execute POST (no retry for POST)
  uint16_t status_code = 0;
  size_t body_size = 0;
  status = _http_action(1, request.timeouts.connection_timeout_seconds,
                        request.timeouts.response_timeout_seconds, &status_code, &body_size);
  if (status != CellularStatus::Ok) {
    _http_terminate();
    return status;
  }

  // Set status code and body size
  out_response->status_code = status_code;
  out_response->body_size = body_size;

  // Read response body if buffer provided and body exists
  if (body_size > 0 && request.response_body.data != nullptr && request.response_body.size > 0) {
    size_t bytes_written = 0;
    bool is_truncated = false;

    status = _http_read_body(request.response_body, body_size, &bytes_written, &is_truncated);
    if (status != CellularStatus::Ok) {
      _http_terminate();
      return status;
    }

    out_response->body_bytes_written = bytes_written;
    out_response->is_body_truncated = is_truncated;
  }

  // Terminate HTTP
  _http_terminate();

  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::_http_init() {
  _at_command_handler.send_at("+HTTPINIT");
  ATCommandHandler::Response response = _at_command_handler.wait_response(HTTP_INIT_TIMEOUT_MS);

  // Retry once on error (module may be in inconsistent state)
  if (response == ATCommandHandler::Response::Expected2) { // ERROR
    RTOS::delay_ms(HTTP_INIT_RETRY_DELAY_MS);
    _at_command_handler.send_at("+HTTPINIT");
    response = _at_command_handler.wait_response(HTTP_INIT_TIMEOUT_MS);
  }

  return _map_at_response_to_status(response);
}

CellularStatus SimcomA7672x::_http_terminate() {
  _at_command_handler.send_at("+HTTPTERM");
  _at_command_handler.wait_response(); // Best-effort cleanup
  return CellularStatus::Ok;           // Always return Ok (fire-and-forget)
}

CellularStatus SimcomA7672x::_http_set_timeouts(int32_t connection_timeout_seconds,
                                                int32_t response_timeout_seconds) {
  // Resolve connection timeout
  const int32_t conn_timeout =
      _resolve_timeout(connection_timeout_seconds, DEFAULT_HTTP_CONNECTION_TIMEOUT_SECONDS,
                       MIN_HTTP_CONNECTION_TIMEOUT_SECONDS, MAX_HTTP_CONNECTION_TIMEOUT_SECONDS);

  // Only set if not default (optimization)
  if (connection_timeout_seconds != CELLULAR_TIMEOUT_USE_DRIVER_DEFAULT) {
    char command[64] = {};
    ::snprintf(command, sizeof(command), "+HTTPPARA=\"CONNECTTO\",%d",
               static_cast<int>(conn_timeout));
    _at_command_handler.send_at(command);
    const ATCommandHandler::Response response = _at_command_handler.wait_response();
    if (response != ATCommandHandler::Response::Expected1) {
      return _map_at_response_to_status(response);
    }
  }

  // Resolve response timeout
  const int32_t resp_timeout =
      _resolve_timeout(response_timeout_seconds, DEFAULT_HTTP_RESPONSE_TIMEOUT_SECONDS,
                       MIN_HTTP_RESPONSE_TIMEOUT_SECONDS, MAX_HTTP_RESPONSE_TIMEOUT_SECONDS);

  // Only set if not default
  if (response_timeout_seconds != CELLULAR_TIMEOUT_USE_DRIVER_DEFAULT) {
    char command[64] = {};
    ::snprintf(command, sizeof(command), "+HTTPPARA=\"RECVTO\",%d", static_cast<int>(resp_timeout));
    _at_command_handler.send_at(command);
    const ATCommandHandler::Response response = _at_command_handler.wait_response();
    if (response != ATCommandHandler::Response::Expected1) {
      return _map_at_response_to_status(response);
    }
  }

  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::_http_set_url(const char *url) {
  char command[256] = {};
  ::snprintf(command, sizeof(command), "+HTTPPARA=\"URL\",\"%s\"", url);
  _at_command_handler.send_at(command);
  const ATCommandHandler::Response response = _at_command_handler.wait_response();
  return _map_at_response_to_status(response);
}

CellularStatus SimcomA7672x::_http_set_content_type(const char *content_type) {
  char command[128] = {};
  ::snprintf(command, sizeof(command), "+HTTPPARA=\"CONTENT\",\"%s\"", content_type);
  _at_command_handler.send_at(command);
  const ATCommandHandler::Response response = _at_command_handler.wait_response();
  return _map_at_response_to_status(response);
}

CellularStatus SimcomA7672x::_http_action(int method_code, int32_t connection_timeout_seconds,
                                          int32_t response_timeout_seconds,
                                          uint16_t *out_status_code, size_t *out_body_size) {
  // Send HTTPACTION command
  char command[32] = {};
  ::snprintf(command, sizeof(command), "+HTTPACTION=%d", method_code);
  _at_command_handler.send_at(command);

  // Wait for immediate OK
  ATCommandHandler::Response response = _at_command_handler.wait_response();
  if (response != ATCommandHandler::Response::Expected1) {
    return _map_at_response_to_status(response);
  }

  // Calculate timeout for URC response
  const uint32_t action_timeout_ms =
      _calculate_http_action_timeout_ms(connection_timeout_seconds, response_timeout_seconds);

  // Wait for +HTTPACTION: URC
  response = _at_command_handler.wait_response(action_timeout_ms, "+HTTPACTION:");
  if (response != ATCommandHandler::Response::Expected1) {
    return _map_at_response_to_status(response);
  }

  // Read the response line: "<method>,<status_code>,<body_size>"
  char response_line[64] = {};
  const CellularStatus line_status =
      _at_command_handler.wait_and_read_line({response_line, sizeof(response_line)});
  if (line_status != CellularStatus::Ok) {
    return line_status;
  }

  // Parse: skip method code, extract status_code and body_size
  int method = 0;
  int status_code = 0;
  int body_size = 0;
  if (::sscanf(response_line, "%d,%d,%d", &method, &status_code, &body_size) != 3) {
    return CellularStatus::Failed;
  }

  // Check for module error codes (700-720)
  if (status_code >= HTTP_MODULE_ERROR_CODE_MIN && status_code <= HTTP_MODULE_ERROR_CODE_MAX) {
    return CellularStatus::Failed;
  }

  *out_status_code = static_cast<uint16_t>(status_code);
  *out_body_size = static_cast<size_t>(body_size);

  _drain_terminal_response();
  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::_http_read_body(MutableBuffer response_buffer, size_t body_size,
                                             size_t *out_bytes_written, bool *out_is_truncated) {
  size_t offset = 0;
  size_t bytes_written = 0;
  bool is_truncated = false;
  char chunk_buffer[HTTP_READ_CHUNK_SIZE] = {};

  while (offset < body_size) {
    // Calculate chunk size for this iteration
    const size_t chunk_size =
        (offset + HTTP_READ_CHUNK_SIZE > body_size) ? (body_size - offset) : HTTP_READ_CHUNK_SIZE;

    // Send HTTPREAD command
    char command[64] = {};
    ::snprintf(command, sizeof(command), "+HTTPREAD=%zu,%zu", offset, chunk_size);
    _at_command_handler.send_at(command);

    // Wait for +HTTPREAD: response
    ATCommandHandler::Response response = _at_command_handler.wait_response("+HTTPREAD:");
    if (response != ATCommandHandler::Response::Expected1) {
      return _map_at_response_to_status(response);
    }

    // Read length line
    char length_line[32] = {};
    CellularStatus line_status =
        _at_command_handler.wait_and_read_line({length_line, sizeof(length_line)});
    if (line_status != CellularStatus::Ok) {
      return line_status;
    }

    // Parse actual length returned
    int actual_length = 0;
    if (::sscanf(length_line, "%d", &actual_length) != 1 || actual_length < 0) {
      return CellularStatus::Failed;
    }

    // Retrieve binary data
    size_t retrieved = 0;
    const CellularStatus retrieve_status = _at_command_handler.retrieve_buffer(
        {reinterpret_cast<uint8_t *>(chunk_buffer), sizeof(chunk_buffer)},
        static_cast<size_t>(actual_length), &retrieved);
    if (retrieve_status != CellularStatus::Ok || retrieved != static_cast<size_t>(actual_length)) {
      return CellularStatus::Failed;
    }

    // Copy to output buffer if there's space
    if (bytes_written + retrieved <= response_buffer.size) {
      ::memcpy(response_buffer.data + bytes_written, chunk_buffer, retrieved);
      bytes_written += retrieved;
    } else {
      // Partial copy if some space remains
      const size_t space_remaining = response_buffer.size - bytes_written;
      if (space_remaining > 0) {
        ::memcpy(response_buffer.data + bytes_written, chunk_buffer, space_remaining);
        bytes_written += space_remaining;
      }
      is_truncated = true;
    }

    // Wait for end marker: +HTTPREAD: 0
    response = _at_command_handler.wait_response("+HTTPREAD: 0");
    if (response != ATCommandHandler::Response::Expected1) {
      return _map_at_response_to_status(response);
    }

    _at_command_handler.clear_buffer();
    offset += chunk_size;
  }

  *out_bytes_written = bytes_written;
  *out_is_truncated = is_truncated;
  return CellularStatus::Ok;
}

int32_t SimcomA7672x::_resolve_timeout(int32_t user_timeout, int32_t default_timeout,
                                       int32_t min_timeout, int32_t max_timeout) const {
  if (user_timeout == CELLULAR_TIMEOUT_USE_DRIVER_DEFAULT) {
    return default_timeout;
  }
  if (user_timeout < min_timeout) {
    return min_timeout;
  }
  if (user_timeout > max_timeout) {
    return max_timeout;
  }
  return user_timeout;
}

uint32_t SimcomA7672x::_calculate_http_action_timeout_ms(int32_t connection_timeout_seconds,
                                                         int32_t response_timeout_seconds) const {
  const int32_t conn_timeout =
      _resolve_timeout(connection_timeout_seconds, DEFAULT_HTTP_CONNECTION_TIMEOUT_SECONDS,
                       MIN_HTTP_CONNECTION_TIMEOUT_SECONDS, MAX_HTTP_CONNECTION_TIMEOUT_SECONDS);
  const int32_t resp_timeout =
      _resolve_timeout(response_timeout_seconds, DEFAULT_HTTP_RESPONSE_TIMEOUT_SECONDS,
                       MIN_HTTP_RESPONSE_TIMEOUT_SECONDS, MAX_HTTP_RESPONSE_TIMEOUT_SECONDS);

  return static_cast<uint32_t>((conn_timeout + resp_timeout) * 1000);
}

bool SimcomA7672x::_has_power_control() const {
  return _hal_gpio != nullptr && _power_pin != gpio::INVALID_PIN;
}

CellularStatus SimcomA7672x::_map_at_response_to_status(ATCommandHandler::Response response) const {
  switch (response) {
  case ATCommandHandler::Response::Expected1:
    return CellularStatus::Ok;
  case ATCommandHandler::Response::Timeout:
    return CellularStatus::Timeout;
  case ATCommandHandler::Response::Expected2:
  case ATCommandHandler::Response::Expected3:
  case ATCommandHandler::Response::CmxError:
    return CellularStatus::Error;
  case ATCommandHandler::Response::BufferOverflow:
    return CellularStatus::BufferTooSmall;
  }

  return CellularStatus::Error;
}

int SimcomA7672x::_map_cellular_technology_to_mode(CellularTechnology technology) const {
  switch (technology) {
  case CellularTechnology::Auto:
    return 2;
  case CellularTechnology::TwoG:
    return 13;
  case CellularTechnology::Lte:
    return 38;
  }

  return -1;
}

const char *SimcomA7672x::_map_cellular_technology_to_registration_command(
    CellularTechnology technology) const {
  switch (technology) {
  case CellularTechnology::Auto:
    return "CREG";
  case CellularTechnology::TwoG:
    return "CGREG";
  case CellularTechnology::Lte:
    return "CEREG";
  }

  return nullptr;
}

CellularRegistrationState SimcomA7672x::_map_registration_state(int stat) const {
  switch (stat) {
  case 0:
    return CellularRegistrationState::NotRegistered;
  case 1:
    return CellularRegistrationState::RegisteredHome;
  case 2:
    return CellularRegistrationState::Searching;
  case 3:
    return CellularRegistrationState::Denied;
  case 5:
    return CellularRegistrationState::RegisteredRoaming;
  case 11:
    return CellularRegistrationState::EmergencyOnly;
  default:
    return CellularRegistrationState::Unknown;
  }
}

CellularStatus SimcomA7672x::_send_at_expect_ok(const char *command, uint32_t timeout_ms) {
  if (command == nullptr) {
    return CellularStatus::InvalidArgument;
  }

  _at_command_handler.send_at(command);
  const ATCommandHandler::Response response = _at_command_handler.wait_response(timeout_ms);
  switch (response) {
  case ATCommandHandler::Response::Expected1:
    return CellularStatus::Ok;
  case ATCommandHandler::Response::Timeout:
    return CellularStatus::Timeout;
  case ATCommandHandler::Response::BufferOverflow:
    return CellularStatus::BufferTooSmall;
  case ATCommandHandler::Response::Expected2:
  case ATCommandHandler::Response::Expected3:
  case ATCommandHandler::Response::CmxError:
    return CellularStatus::Error;
  }

  return CellularStatus::Error;
}

CellularStatus SimcomA7672x::_send_raw_expect_ok(const char *command, uint32_t timeout_ms) {
  if (command == nullptr) {
    return CellularStatus::InvalidArgument;
  }

  _at_command_handler.send_raw(command);
  return _map_at_response_to_status(_at_command_handler.wait_response(timeout_ms));
}

CellularStatus SimcomA7672x::_disable_network_registration_urc(CellularTechnology technology) {
  if (technology == CellularTechnology::Auto) {
    CellularStatus status = _send_at_expect_ok("+CREG=0");
    if (status != CellularStatus::Ok) {
      return status;
    }

    status = _send_at_expect_ok("+CGREG=0");
    if (status != CellularStatus::Ok) {
      return status;
    }

    return _send_at_expect_ok("+CEREG=0");
  }

  const char *registration_command = _map_cellular_technology_to_registration_command(technology);
  if (registration_command == nullptr) {
    return CellularStatus::Unsupported;
  }

  char command[16] = {};
  snprintf(command, sizeof(command), "+%s=0", registration_command);
  return _send_at_expect_ok(command);
}

CellularStatus SimcomA7672x::_apply_cellular_technology(CellularTechnology technology) {
  const int mode = _map_cellular_technology_to_mode(technology);
  if (mode < 0) {
    return CellularStatus::Unsupported;
  }

  char command[16] = {};
  snprintf(command, sizeof(command), "+CNMP=%d", mode);
  return _send_at_expect_ok(command);
}

CellularStatus SimcomA7672x::_apply_apn(const char *apn) {
  if (apn == nullptr || apn[0] == '\0') {
    return CellularStatus::InvalidArgument;
  }

  char command[128] = {};
  snprintf(command, sizeof(command), "+CGDCONT=1,\"IP\",\"%s\"", apn);
  return _send_at_expect_ok(command);
}

CellularStatus SimcomA7672x::_is_service_available(bool *out_service_available) {
  if (out_service_available == nullptr) {
    return CellularStatus::InvalidArgument;
  }

  _at_command_handler.send_at("+CNSMOD?");
  if (_at_command_handler.wait_response(ATCommandHandler::DEFAULT_WAIT_RESPONSE_TIMEOUT_MS,
                                        "+CNSMOD:") != ATCommandHandler::Response::Expected1) {
    return CellularStatus::Timeout;
  }

  char service_status[32] = {};
  const CellularStatus line_status =
      _at_command_handler.wait_and_read_line({service_status, sizeof(service_status)});
  if (line_status != CellularStatus::Ok) {
    return line_status;
  }

  _drain_terminal_response();
  *out_service_available =
      (::strcmp(service_status, "0,0") != 0 && ::strcmp(service_status, "1,0") != 0);
  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::_activate_pdp_context() { return _send_at_expect_ok("+CGACT=1,1"); }

CellularStatus SimcomA7672x::_ensure_packet_domain_attached(bool force_attach,
                                                            bool *out_is_attached) {
  if (out_is_attached == nullptr) {
    return CellularStatus::InvalidArgument;
  }

  _at_command_handler.send_at("+CGATT?");
  if (_at_command_handler.wait_response(ATCommandHandler::DEFAULT_WAIT_RESPONSE_TIMEOUT_MS,
                                        "+CGATT:") != ATCommandHandler::Response::Expected1) {
    return CellularStatus::Error;
  }

  char attach_state[8] = {};
  const CellularStatus line_status =
      _at_command_handler.wait_and_read_line({attach_state, sizeof(attach_state)});
  if (line_status != CellularStatus::Ok) {
    return line_status;
  }

  _drain_terminal_response();
  if (::strcmp(attach_state, "1") == 0) {
    *out_is_attached = true;
    return CellularStatus::Ok;
  }

  *out_is_attached = false;
  if (!force_attach) {
    return CellularStatus::Failed;
  }

  const CellularStatus attach_status = _send_at_expect_ok("+CGATT=1");
  if (attach_status == CellularStatus::Ok) {
    *out_is_attached = true;
  }
  return attach_status;
}

CellularStatus SimcomA7672x::_parse_network_registration_line(
    const char *line, CellularNetworkRegistration *out_registration) const {
  if (line == nullptr || out_registration == nullptr) {
    return CellularStatus::InvalidArgument;
  }

  char line_copy[64] = {};
  if (strlen(line) >= sizeof(line_copy)) {
    return CellularStatus::BufferTooSmall;
  }

  strcpy(line_copy, line);

  char *first_separator = strchr(line_copy, ',');
  if (first_separator == nullptr) {
    return CellularStatus::Failed;
  }

  *first_separator = '\0';
  char *parse_end = nullptr;
  const long mode = strtol(line_copy, &parse_end, 10);
  if (parse_end == line_copy || *parse_end != '\0') {
    return CellularStatus::Failed;
  }

  char *stat_token = first_separator + 1;
  char *second_separator = strchr(stat_token, ',');
  if (second_separator != nullptr) {
    *second_separator = '\0';
  }

  parse_end = nullptr;
  const long stat = strtol(stat_token, &parse_end, 10);
  if (parse_end == stat_token || *parse_end != '\0') {
    return CellularStatus::Failed;
  }

  (void)mode;
  out_registration->state = _map_registration_state(static_cast<int>(stat));
  out_registration->operator_id = 0;
  out_registration->is_packet_service_ready =
      out_registration->state == CellularRegistrationState::RegisteredHome ||
      out_registration->state == CellularRegistrationState::RegisteredRoaming;
  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::_scan_available_operators(OperatorList *out_operators,
                                                       uint32_t timeout_ms,
                                                       MutableStringBuffer work_buffer) {
  if (out_operators == nullptr || work_buffer.data == nullptr || work_buffer.size == 0) {
    return CellularStatus::InvalidArgument;
  }

  out_operators->count = 0;

  _at_command_handler.send_at("+COPS=?");
  const ATCommandHandler::Response response =
      _at_command_handler.wait_response(timeout_ms, "+COPS:");
  if (response != ATCommandHandler::Response::Expected1) {
    return _map_at_response_to_status(response);
  }

  size_t line_length = 0;
  const CellularStatus line_status =
      _at_command_handler.wait_and_read_line(work_buffer, &line_length);
  if (line_status != CellularStatus::Ok) {
    return line_status;
  }

  _drain_terminal_response();

  return _parse_operator_list(work_buffer.data, out_operators);
}

CellularStatus SimcomA7672x::_parse_operator_list(const char *response,
                                                  OperatorList *out_operators) const {
  if (response == nullptr || out_operators == nullptr) {
    return CellularStatus::InvalidArgument;
  }

  out_operators->count = 0;
  const char *pos = response;

  while (*pos != '\0' && out_operators->count < OperatorList::MAX_OPERATORS) {
    // Find opening parenthesis
    const char *open_paren = ::strchr(pos, '(');
    if (open_paren == nullptr) {
      break;
    }

    // Find closing parenthesis
    const char *close_paren = ::strchr(open_paren, ')');
    if (close_paren == nullptr) {
      break;
    }

    // Parse entry: (status,"long","short","numeric",tech)
    // We need status (pos 0), numeric (pos 3), tech (pos 4)
    const char *entry_start = open_paren + 1;
    int field_index = 0;
    int status = -1;
    uint32_t operator_id = 0;
    int access_tech = -1;
    bool in_quotes = false;
    const char *field_start = entry_start;

    for (const char *p = entry_start; p < close_paren; ++p) {
      if (*p == '"') {
        in_quotes = !in_quotes;
      } else if (*p == ',' && !in_quotes) {
        // End of field
        if (field_index == 0) {
          // Parse status
          status = static_cast<int>(::strtol(field_start, nullptr, 10));
        } else if (field_index == 3) {
          // Parse numeric operator ID (skip quotes if present)
          const char *num_start = field_start;
          if (*num_start == '"') {
            num_start++;
          }
          operator_id = static_cast<uint32_t>(::strtoul(num_start, nullptr, 10));
        }
        field_index++;
        field_start = p + 1;
      }
    }

    // Parse last field (tech)
    if (field_index == 4) {
      access_tech = static_cast<int>(::strtol(field_start, nullptr, 10));
    }

    // Only include available (1) or current (2) operators
    if ((status == 1 || status == 2) && operator_id > 0) {
      OperatorInfo &op = out_operators->operators[out_operators->count];
      op.operator_id = operator_id;
      switch (access_tech) {
      case 0:
        op.access_tech = OperatorAccessTech::Gsm;
        break;
      case 2:
        op.access_tech = OperatorAccessTech::Utran;
        break;
      case 7:
        op.access_tech = OperatorAccessTech::EUtranLte;
        break;
      default:
        op.access_tech = OperatorAccessTech::Unknown;
        break;
      }
      out_operators->count++;
    }

    pos = close_paren + 1;
  }

  return CellularStatus::Ok;
}

CellularStatus SimcomA7672x::_apply_operator_selection(uint32_t operator_id,
                                                       OperatorAccessTech access_tech) {
  char command[48] = {};

  if (operator_id == 0) {
    // Automatic operator selection
    snprintf(command, sizeof(command), "+COPS=0,2");
  } else {
    // Manual operator selection with numeric format
    int tech_value = static_cast<int>(access_tech);
    if (access_tech == OperatorAccessTech::Unknown || tech_value < 0) {
      // No access tech specified, let modem choose
      snprintf(command, sizeof(command), "+COPS=1,2,\"%lu\"",
               static_cast<unsigned long>(operator_id));
    } else {
      snprintf(command, sizeof(command), "+COPS=1,2,\"%lu\",%d",
               static_cast<unsigned long>(operator_id), tech_value);
    }
  }

  _at_command_handler.send_at(command);
  const ATCommandHandler::Response response =
      _at_command_handler.wait_response(OPERATOR_SELECTION_TIMEOUT_MS);

  return _map_at_response_to_status(response);
}

SimcomA7672x::NetworkRegistrationState SimcomA7672x::_impl_check_module_ready() {
  if (!_at_command_handler.test_at()) {
    RTOS::delay_ms(REGISTRATION_RETRY_DELAY_MS);
    return NetworkRegistrationState::CheckModuleReady;
  }

  const CellularStatus sim_status = check_sim_ready();
  if (sim_status != CellularStatus::Ok) {
    RTOS::delay_ms(REGISTRATION_RETRY_DELAY_MS);
    return NetworkRegistrationState::CheckModuleReady;
  }

  return NetworkRegistrationState::PrepareModule;
}

SimcomA7672x::NetworkRegistrationState
SimcomA7672x::_impl_prepare_module(CellularTechnology technology, const char *apn) {
  CellularStatus status = _disable_network_registration_urc(technology);
  if (status == CellularStatus::Timeout) {
    return NetworkRegistrationState::CheckModuleReady;
  }
  if (status != CellularStatus::Ok) {
    // Fatal error - will be handled by caller returning the status
    return NetworkRegistrationState::CheckModuleReady;
  }

  status = _apply_cellular_technology(technology);
  if (status == CellularStatus::Timeout) {
    return NetworkRegistrationState::CheckModuleReady;
  }
  if (status != CellularStatus::Ok) {
    return NetworkRegistrationState::CheckModuleReady;
  }

  status = _apply_apn(apn);
  if (status == CellularStatus::Timeout) {
    return NetworkRegistrationState::CheckModuleReady;
  }
  if (status != CellularStatus::Ok) {
    return NetworkRegistrationState::CheckModuleReady;
  }

  // Check if we have cached operator list
  if (_scanned_operators.count > 0) {
    return NetworkRegistrationState::ConfigureManualNetwork;
  } else {
    return NetworkRegistrationState::ScanOperators;
  }
}

SimcomA7672x::NetworkRegistrationState
SimcomA7672x::_impl_scan_operators(uint32_t scan_timeout_ms, MutableStringBuffer work_buffer) {
  CellularStatus status =
      _scan_available_operators(&_scanned_operators, scan_timeout_ms, work_buffer);
  if (status == CellularStatus::Timeout) {
    return NetworkRegistrationState::CheckModuleReady;
  }
  if (status != CellularStatus::Ok || _scanned_operators.count == 0) {
    return NetworkRegistrationState::OperatorListExhausted;
  }
  _current_operator_index = 0;
  return NetworkRegistrationState::ConfigureManualNetwork;
}

SimcomA7672x::NetworkRegistrationState
SimcomA7672x::_impl_configure_manual_network(uint64_t *out_registration_check_start_ms) {
  // If we have a saved operator and this is our first attempt (index 0),
  // search for it in the list and try it first
  if (_last_successful_operator.operator_id != 0 && _current_operator_index == 0) {
    for (size_t i = 0; i < _scanned_operators.count; i++) {
      if (_scanned_operators.operators[i].operator_id == _last_successful_operator.operator_id &&
          _scanned_operators.operators[i].access_tech == _last_successful_operator.access_tech) {
        _current_operator_index = i;
        break;
      }
    }
  }

  // Check if we've exhausted all operators
  if (_current_operator_index >= _scanned_operators.count) {
    return NetworkRegistrationState::OperatorListExhausted;
  }

  const OperatorInfo &op = _scanned_operators.operators[_current_operator_index];
  CellularStatus status = _apply_operator_selection(op.operator_id, op.access_tech);
  if (status == CellularStatus::Ok) {
    *out_registration_check_start_ms = RTOS::get_time_ms();
    return NetworkRegistrationState::CheckNetworkRegistration;
  }
  if (status == CellularStatus::Timeout) {
    return NetworkRegistrationState::CheckModuleReady;
  }
  // Failed or Error - try next operator
  _current_operator_index++;
  if (_current_operator_index < _scanned_operators.count) {
    return NetworkRegistrationState::ConfigureManualNetwork;
  }
  return NetworkRegistrationState::OperatorListExhausted;
}

SimcomA7672x::NetworkRegistrationState
SimcomA7672x::_impl_check_network_registration(CellularTechnology technology,
                                               uint64_t registration_check_start_ms,
                                               uint64_t *out_service_status_start_ms) {
  CellularNetworkRegistration registration = {};
  CellularStatus status = get_network_registration(technology, &registration);
  if (status == CellularStatus::Timeout) {
    return NetworkRegistrationState::CheckModuleReady;
  }
  if (status != CellularStatus::Ok) {
    RTOS::delay_ms(REGISTRATION_RETRY_DELAY_MS);
    return NetworkRegistrationState::CheckNetworkRegistration;
  }

  if (!registration.is_packet_service_ready) {
    // Check per-operator timeout
    if ((RTOS::get_time_ms() - registration_check_start_ms) > REGISTRATION_CHECK_TIMEOUT_MS) {
      // Timeout - try next operator
      _current_operator_index++;
      if (_current_operator_index < _scanned_operators.count) {
        return NetworkRegistrationState::ConfigureManualNetwork;
      }
      return NetworkRegistrationState::OperatorListExhausted;
    }
    RTOS::delay_ms(REGISTRATION_POLL_DELAY_MS);
    return NetworkRegistrationState::CheckNetworkRegistration;
  }

  CellularSignalQuality signal_quality = {};
  status = get_signal_quality(&signal_quality);
  if (status == CellularStatus::Timeout) {
    return NetworkRegistrationState::CheckModuleReady;
  }
  if (status != CellularStatus::Ok || signal_quality.csq < MIN_VALID_SIGNAL_CSQ ||
      signal_quality.csq > MAX_VALID_SIGNAL_CSQ || signal_quality.csq < MIN_READY_SIGNAL_CSQ) {
    // Check per-operator timeout
    if ((RTOS::get_time_ms() - registration_check_start_ms) > REGISTRATION_CHECK_TIMEOUT_MS) {
      // Timeout - try next operator
      _current_operator_index++;
      if (_current_operator_index < _scanned_operators.count) {
        return NetworkRegistrationState::ConfigureManualNetwork;
      }
      return NetworkRegistrationState::OperatorListExhausted;
    }
    RTOS::delay_ms(REGISTRATION_RETRY_DELAY_MS);
    return NetworkRegistrationState::CheckNetworkRegistration;
  }

  // Save successful operator
  if (_scanned_operators.count > 0 && _current_operator_index < _scanned_operators.count) {
    _last_successful_operator = _scanned_operators.operators[_current_operator_index];
  }

  *out_service_status_start_ms = RTOS::get_time_ms();
  return NetworkRegistrationState::CheckServiceStatus;
}

SimcomA7672x::NetworkRegistrationState
SimcomA7672x::_impl_check_service_status(uint64_t service_status_start_ms) {
  bool is_service_available = false;
  CellularStatus status = _is_service_available(&is_service_available);
  if (status == CellularStatus::Timeout) {
    return NetworkRegistrationState::CheckModuleReady;
  }
  if (status != CellularStatus::Ok || !is_service_available) {
    if ((RTOS::get_time_ms() - service_status_start_ms) > SERVICE_STATUS_TIMEOUT_MS) {
      return NetworkRegistrationState::CheckNetworkRegistration;
    }
    RTOS::delay_ms(REGISTRATION_RETRY_DELAY_MS);
    return NetworkRegistrationState::CheckServiceStatus;
  }

  status = _activate_pdp_context();
  if (status == CellularStatus::Timeout) {
    return NetworkRegistrationState::CheckModuleReady;
  }
  if (status != CellularStatus::Ok) {
    RTOS::delay_ms(REGISTRATION_RETRY_DELAY_MS);
    return NetworkRegistrationState::CheckServiceStatus;
  }

  bool is_attached = false;
  status = _ensure_packet_domain_attached(true, &is_attached);
  if (status == CellularStatus::Timeout) {
    return NetworkRegistrationState::CheckModuleReady;
  }
  if (status != CellularStatus::Ok || !is_attached) {
    RTOS::delay_ms(REGISTRATION_RETRY_DELAY_MS);
    return NetworkRegistrationState::CheckServiceStatus;
  }

  return NetworkRegistrationState::NetworkReady;
}

CellularStatus SimcomA7672x::_impl_network_ready() {
  CellularSignalQuality signal_quality = {};
  CellularStatus status = get_signal_quality(&signal_quality);
  if (status == CellularStatus::Timeout) {
    return CellularStatus::Timeout;
  }
  if (status != CellularStatus::Ok || signal_quality.csq < MIN_VALID_SIGNAL_CSQ ||
      signal_quality.csq > MAX_VALID_SIGNAL_CSQ) {
    RTOS::delay_ms(REGISTRATION_RETRY_DELAY_MS);
    return CellularStatus::Failed;
  }

  char ip_address[32] = {};
  size_t ip_address_length = 0;
  status = get_ip_address({ip_address, sizeof(ip_address)}, &ip_address_length);
  if (status != CellularStatus::Ok || ip_address_length == 0) {
    RTOS::delay_ms(REGISTRATION_RETRY_DELAY_MS);
    return CellularStatus::Failed;
  }

  if (_warm_up_time_ms > 0) {
    RTOS::delay_ms(_warm_up_time_ms);
  }
  return CellularStatus::Ok;
}

void SimcomA7672x::_drain_terminal_response() { (void)_at_command_handler.wait_response(); }

int SimcomA7672x::_csq_to_rssi_dbm(int csq) const {
  if (csq < 0 || csq > 31) {
    return CELLULAR_RSSI_DBM_INVALID;
  }

  return -113 + (2 * csq);
}
