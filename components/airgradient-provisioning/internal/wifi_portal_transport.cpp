/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "wifi_portal_transport.h"

#include <algorithm>
#include <cstdio>
#include <cstring>

#include <cJSON.h>

#include "ag_log.h"
#include "hal/http_request.h"
#include "hal/http_response.h"
#include "hal/http_server.h"
#include "scan_filter.h"
#include "types/http_types.h"

namespace {

constexpr const char *TAG = "WifiPortal";

const char *auth_mode_string(WifiAuthMode m) {
  switch (m) {
  case WifiAuthMode::open:
    return "open";
  case WifiAuthMode::wep:
    return "wep";
  case WifiAuthMode::wpa_psk:
    return "wpa_psk";
  case WifiAuthMode::wpa2_psk:
    return "wpa2_psk";
  case WifiAuthMode::wpa_wpa2_psk:
    return "wpa_wpa2_psk";
  case WifiAuthMode::wpa3_psk:
    return "wpa3_psk";
  case WifiAuthMode::wpa2_wpa3_psk:
    return "wpa2_wpa3_psk";
  case WifiAuthMode::wapi_psk:
    return "wapi_psk";
  case WifiAuthMode::owe:
    return "owe";
  case WifiAuthMode::unknown:
  default:
    return "unknown";
  }
}

const char *portal_state_string(WifiPortalTransport::PortalState s) {
  switch (s) {
  case WifiPortalTransport::PortalState::Waiting:
    return "waiting";
  case WifiPortalTransport::PortalState::Connecting:
    return "connecting";
  case WifiPortalTransport::PortalState::Connected:
    return "connected";
  case WifiPortalTransport::PortalState::Failed:
    return "failed";
  }
  return "waiting";
}

// Parse a dotted-decimal IPv4 string into network byte order (octet 0 in
// the low byte, matching lwIP / WifiStaticIpConfig). Returns true on
// success.
bool parse_ipv4(const char *s, uint32_t &out_be) {
  if (s == nullptr) {
    return false;
  }
  unsigned a = 0, b = 0, c = 0, d = 0;
  // sscanf returns the number of fields parsed.
  int matched = std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d);
  if (matched != 4) {
    return false;
  }
  if (a > 255 || b > 255 || c > 255 || d > 255) {
    return false;
  }
  out_be = (static_cast<uint32_t>(a)) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
  return true;
}

void send_json(HttpResponse &resp, HttpStatus status, cJSON *root) {
  char *encoded = cJSON_PrintUnformatted(root);
  if (encoded == nullptr) {
    resp.json(HttpStatus::InternalServerError, R"({"error":"encode"})");
    return;
  }
  resp.json(status, encoded);
  cJSON_free(encoded);
}

} // namespace

namespace {

// Well-known OS captive-portal probe URLs. Each gets a GET handler that
// returns 302 Found → "/" so the probing OS opens its in-app captive
// browser at the actual portal page instead of treating the AP as a
// broken network. Covers iOS, macOS, Android (incl. OEM variants),
// Windows, Firefox, and a few NetworkManager variants.
constexpr const char *CAPTIVE_PROBE_PATHS[] = {
    "/hotspot-detect.html",       // iOS / macOS
    "/library/test/success.html", // iOS legacy
    "/generate_204",              // Android / Chrome
    "/gen_204",                   // Android variant
    "/connecttest.txt",           // Windows 10/11
    "/ncsi.txt",                  // Windows legacy
    "/canonical.html",            // Firefox
    "/redirect",                  // Some Android OEMs
    "/success.txt",               // Linux NetworkManager
};

} // namespace

void WifiPortalTransport::handle_captive_probe(const HttpRequest &req, HttpResponse &resp) {
  AG_LOGD(TAG, "captive probe hit: %s", req.uri());

  // 302 Found + absolute Location URL pointing directly at the AP.
  // iOS CNA (and similar OS captive-portal detectors) treat a probe
  // response as a captive signal only when the Location is a fully
  // qualified URL to a different host. A relative redirect like
  // `Location: /` is followed silently and the popup never appears.
  //
  // The AP IP is the lwIP soft-AP default (192.168.4.1); keep this in
  // sync with DEFAULT_AP_IP_BE in provisioning_manager.cpp if it ever
  // changes.
  resp.set_header("Location", "http://192.168.4.1/");
  // Body is irrelevant for a redirect, but we set content-type to avoid
  // the response defaulting to 500's content type via HttpResponse.
  resp.body(HttpStatus::Found, "", 0, "text/plain");
}

bool WifiPortalTransport::register_routes(HttpServer &http, const uint8_t *html_start,
                                          const uint8_t *html_end) {
  bool ok = true;
  if (html_start != nullptr && html_end != nullptr && html_end > html_start) {
    ok &= http.register_static("/", html_start, html_end, "text/html");
    ok &= http.register_static("/index.html", html_start, html_end, "text/html");
  }
  ok &= http.register_route(
      HttpMethod::Post, "/api/scan",
      [this](const HttpRequest &q, HttpResponse &r) { handle_scan_post(q, r); });
  ok &=
      http.register_route(HttpMethod::Get, "/api/scan",
                          [this](const HttpRequest &q, HttpResponse &r) { handle_scan_get(q, r); });
  ok &= http.register_route(
      HttpMethod::Post, "/api/provision",
      [this](const HttpRequest &q, HttpResponse &r) { handle_provision_post(q, r); });
  ok &= http.register_route(
      HttpMethod::Get, "/api/status",
      [this](const HttpRequest &q, HttpResponse &r) { handle_status_get(q, r); });

  // OS captive-portal probe URLs — all redirect to the portal page.
  for (const char *path : CAPTIVE_PROBE_PATHS) {
    ok &= http.register_route(HttpMethod::Get, path, &handle_captive_probe);
  }
  // Silence the browser favicon probe (one less 404 in the logs).
  ok &= http.register_route(HttpMethod::Get, "/favicon.ico",
                            [](const HttpRequest &, HttpResponse &r) { r.no_content(); });

  return ok;
}

void WifiPortalTransport::update_scan_results(const WifiScanEntry *entries, uint16_t count) {
  _scan_cache_size = ScanFilter::apply(entries, count, _scan_cache, MAX_CACHED_SCAN);
  _scan_state = ScanState::Done;
  _scan_in_progress = false;
}

void WifiPortalTransport::set_state(PortalState state) { _state = state; }

// ---------------------------------------------------------------------------
// Handlers
// ---------------------------------------------------------------------------

void WifiPortalTransport::handle_scan_post(const HttpRequest & /*req*/, HttpResponse &resp) {
  bool started = false;
  if (_on_scan_request) {
    started = _on_scan_request();
  }
  if (!started) {
    resp.json(HttpStatus::InternalServerError, R"({"status":"error"})");
    return;
  }
  _scan_state = ScanState::Scanning;
  _scan_in_progress = true;
  // Invalidate previous results so a stale list isn't returned mid-scan.
  _scan_cache_size = 0;
  resp.json(HttpStatus::Ok, R"({"status":"scanning"})");
}

void WifiPortalTransport::handle_scan_get(const HttpRequest & /*req*/, HttpResponse &resp) {
  if (_scan_state == ScanState::Idle) {
    resp.json(HttpStatus::Ok, R"({"status":"idle"})");
    return;
  }
  if (_scan_state == ScanState::Scanning) {
    resp.json(HttpStatus::Ok, R"({"status":"scanning"})");
    return;
  }

  cJSON *root = cJSON_CreateObject();
  cJSON *networks = cJSON_CreateArray();
  if (root == nullptr || networks == nullptr) {
    if (root != nullptr)
      cJSON_Delete(root);
    if (networks != nullptr)
      cJSON_Delete(networks);
    resp.json(HttpStatus::InternalServerError, R"({"error":"encode"})");
    return;
  }
  cJSON_AddStringToObject(root, "status", "done");

  for (size_t i = 0; i < _scan_cache_size; ++i) {
    const WifiScanEntry &e = _scan_cache[i];
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "ssid", e.ssid);
    cJSON_AddNumberToObject(entry, "rssi", static_cast<double>(e.rssi));
    cJSON_AddStringToObject(entry, "auth", auth_mode_string(e.auth_mode));
    cJSON_AddNumberToObject(entry, "channel", static_cast<double>(e.channel));
    cJSON_AddItemToArray(networks, entry);
  }
  cJSON_AddItemToObject(root, "networks", networks);

  send_json(resp, HttpStatus::Ok, root);
  cJSON_Delete(root);
}

void WifiPortalTransport::handle_provision_post(const HttpRequest &req, HttpResponse &resp) {
  const char *body = req.body();
  const size_t body_len = req.body_length();
  if (body == nullptr || body_len == 0) {
    resp.json(HttpStatus::BadRequest, R"({"error":"empty body"})");
    return;
  }

  cJSON *root = cJSON_ParseWithLength(body, body_len);
  if (root == nullptr) {
    resp.json(HttpStatus::BadRequest, R"({"error":"invalid json"})");
    return;
  }

  ProvisioningData data;

  cJSON *ssid = cJSON_GetObjectItemCaseSensitive(root, "ssid");
  if (!cJSON_IsString(ssid) || ssid->valuestring == nullptr || ssid->valuestring[0] == '\0') {
    cJSON_Delete(root);
    resp.json(HttpStatus::BadRequest, R"({"error":"missing ssid"})");
    return;
  }
  std::strncpy(data.ssid, ssid->valuestring, sizeof(data.ssid) - 1);

  cJSON *password = cJSON_GetObjectItemCaseSensitive(root, "password");
  if (cJSON_IsString(password) && password->valuestring != nullptr) {
    std::strncpy(data.password, password->valuestring, sizeof(data.password) - 1);
  }

  cJSON *disable_cloud = cJSON_GetObjectItemCaseSensitive(root, "disableCloud");
  if (cJSON_IsBool(disable_cloud)) {
    data.disable_cloud = cJSON_IsTrue(disable_cloud);
  }

  cJSON *static_ip = cJSON_GetObjectItemCaseSensitive(root, "staticIp");
  if (cJSON_IsObject(static_ip)) {
    cJSON *ip_node = cJSON_GetObjectItemCaseSensitive(static_ip, "ip");
    cJSON *netmask_node = cJSON_GetObjectItemCaseSensitive(static_ip, "netmask");
    cJSON *gateway_node = cJSON_GetObjectItemCaseSensitive(static_ip, "gateway");
    cJSON *dns_node = cJSON_GetObjectItemCaseSensitive(static_ip, "dns");

    bool any_invalid = false;
    if (cJSON_IsString(ip_node) && ip_node->valuestring != nullptr) {
      if (!parse_ipv4(ip_node->valuestring, data.static_ip.ip)) {
        any_invalid = true;
      }
    }
    if (cJSON_IsString(netmask_node) && netmask_node->valuestring != nullptr) {
      if (!parse_ipv4(netmask_node->valuestring, data.static_ip.netmask)) {
        any_invalid = true;
      }
    }
    if (cJSON_IsString(gateway_node) && gateway_node->valuestring != nullptr) {
      if (!parse_ipv4(gateway_node->valuestring, data.static_ip.gateway)) {
        any_invalid = true;
      }
    }
    if (cJSON_IsString(dns_node) && dns_node->valuestring != nullptr) {
      if (!parse_ipv4(dns_node->valuestring, data.static_ip.dns_primary)) {
        any_invalid = true;
      }
    }
    if (any_invalid || data.static_ip.ip == 0) {
      // Reject malformed static IP outright — partial DHCP/static
      // mixes lead to confusing failures downstream.
      cJSON_Delete(root);
      resp.json(HttpStatus::BadRequest, R"({"error":"invalid staticIp"})");
      return;
    }
  }

  cJSON_Delete(root);

  bool accepted = false;
  if (_on_credentials) {
    accepted = _on_credentials(data);
  }
  if (!accepted) {
    // Either state machine is busy or no callback wired. Tell the
    // client and let it poll status.
    resp.json(HttpStatus::Ok, R"({"status":"busy"})");
    return;
  }
  resp.json(HttpStatus::Ok, R"({"status":"connecting"})");
}

void WifiPortalTransport::handle_status_get(const HttpRequest & /*req*/, HttpResponse &resp) {
  char buf[64];
  const int n = std::snprintf(buf, sizeof(buf), R"({"state":"%s"})", portal_state_string(_state));
  if (n <= 0) {
    resp.json(HttpStatus::InternalServerError, R"({"error":"snprintf"})");
    return;
  }
  resp.json(HttpStatus::Ok, buf, static_cast<size_t>(n));
}
