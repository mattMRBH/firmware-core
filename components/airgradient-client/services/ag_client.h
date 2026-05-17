/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_CLIENT_H
#define AG_CLIENT_H

#include <cstddef>
#include <string>

#include "../clients/coap_client.h"
#include "../clients/http_client.h"
#include "../clients/mqtt_client.h"
#include "../types/client_types.h"

class CellularModem; // forward declaration; full def only needed when
                     // cellular backends are implemented.

// Unified AirGradient server client.
//
// One object handles all communication with the AirGradient backend over a
// single network transport chosen at boot.  This release implements the WiFi
// HTTP path only; CoAP/MQTT/cellular methods are present but abort with a
// clear error log (see spec.md "Unsupported Combination Handling").
//
// Execution context: task context only
// ISR-safe: no
// Thread-safe: no -- single owner expected
// Blocking: yes (network I/O)
class AgClient {
public:
  AgClient() = default;

  // Initialize the client.  `serial_number` must be a 12-char hex string.
  // For NetworkType::Wifi, `modem` is ignored.  For NetworkType::Cellular,
  // this currently logs an error and returns false (future spec).
  bool begin(const char *serial_number, NetworkType network, CellularModem *modem = nullptr);

  // --- HTTP (WiFi only -- aborts on Cellular) ---
  AgClientResult http_fetch_config(char *config_out, size_t config_size, size_t *bytes_written);
  AgClientResult http_post_measures(const AgClientMeasuresType &measures, int signal);

  // --- CoAP (Cellular only -- aborts on WiFi, currently aborts until
  //          cellular backends are implemented) ---
  AgClientResult coap_fetch_config(char *config_out, size_t config_size, size_t *bytes_written);
  AgClientResult coap_post_measures(const AgClientMeasuresType &measures, int signal,
                                    int interval_seconds);
  AgClientResult coap_post_measures(const AgClientMeasuresType *measures, size_t count, int signal,
                                    int interval_seconds);

  // --- MQTT (future spec -- aborts until backends are implemented) ---
  AgClientResult mqtt_connect(const char *host, int port, const char *username = nullptr,
                              const char *password = nullptr);
  AgClientResult mqtt_disconnect();
  AgClientResult mqtt_publish_measures(const AgClientMeasuresType &measures, int signal,
                                       int interval_seconds);

  // --- Domain override (for staging/testing) ---
  void set_http_domain(const char *domain);
  void reset_http_domain();
  void set_coap_host(const char *host);
  void reset_coap_host();

private:
  static constexpr const char *DEFAULT_HTTP_DOMAIN = "hw.airgradient.com";
  static constexpr const char *DEFAULT_COAP_HOST = "128.140.49.53";
  static constexpr int DEFAULT_COAP_PORT = 5683;
  // Buffer size for the serialized JSON measures POST body.  The full
  // Measures variant with all fields valid fits comfortably in <512 bytes.
  static constexpr size_t POST_BODY_BUFFER_SIZE = 768;
  // Buffer size for the rendered URL.  AG server endpoints are well under
  // 128 bytes (https://hw.airgradient.com/sensors/airgradient:<sn>/measures).
  static constexpr size_t URL_BUFFER_SIZE = 128;

  bool _build_fetch_config_url(char *buf, size_t size) const;
  bool _build_post_measures_url(char *buf, size_t size) const;

  // Map HttpClient::get() outcome to AgClientResult for config fetch.
  static AgClientResult _map_fetch_config_result(bool transport_ok, int status, bool truncated);
  // Map HttpClient::post() outcome to AgClientResult for measures post.
  static AgClientResult _map_post_measures_result(bool transport_ok, int status);

  NetworkType _network = NetworkType::Wifi;
  char _serial_number[13] = {}; // 12-char hex + NUL

  std::string _http_domain = DEFAULT_HTTP_DOMAIN;
  std::string _coap_host = DEFAULT_COAP_HOST;

  HttpClient *_http = nullptr;
  MqttClient *_mqtt = nullptr;
  CoapClient *_coap = nullptr;

#ifdef TEST_HOST
  friend class AgClientTestAccess;
#endif
};

#endif // AG_CLIENT_H
