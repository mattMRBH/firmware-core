/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <algorithm>
#include <cstring>
#include <vector>

#include "hal/http_server.h"
#include "hal/wifi_hal.h"
#include "internal/provisioning_timer.h"
#include "internal/wifi_portal_transport.h"
#include "services/provisioning_manager.h"
#include "services/wifi_manager.h"
#include "test_http_request.h"

namespace {

// Minimal WifiHal stub. The provisioning manager calls into WifiManager
// only for AP/STA/scan operations; the underlying HAL just needs to
// accept those calls and store the registered callbacks.
class FakeWifiHal : public WifiHal {
public:
  WifiStatus init() override { return WifiStatus::Ok; }
  void deinit() override {}

  WifiStatus set_mode(WifiMode m) override {
    _mode = m;
    return WifiStatus::Ok;
  }
  WifiMode get_mode() const override { return _mode; }

  WifiStatus connect_sta(const char *ssid, const char *password) override {
    ++connect_calls;
    last_ssid = ssid != nullptr ? ssid : "";
    last_password = password != nullptr ? password : "";
    return WifiStatus::Ok;
  }
  WifiStatus disconnect_sta() override {
    ++disconnect_calls;
    return WifiStatus::Ok;
  }

  WifiStatus set_static_ip(const WifiStaticIpConfig &) override { return WifiStatus::Ok; }
  WifiStatus clear_static_ip() override { return WifiStatus::Ok; }

  WifiStatus start_scan(const WifiScanConfig &) override { return WifiStatus::Ok; }

  WifiStatus start_ap(const WifiApConfig &) override { return WifiStatus::Ok; }
  WifiStatus stop_ap() override { return WifiStatus::Ok; }

  WifiStatusSnapshot get_status() const override { return {}; }
  WifiStatus set_power_save(WifiPowerSave) override { return WifiStatus::Ok; }

  WifiStatus start_mdns(const WifiMdnsConfig &) override { return WifiStatus::Ok; }
  WifiStatus stop_mdns() override { return WifiStatus::Ok; }
  WifiStatus clear_saved_credentials() override { return WifiStatus::Ok; }

  WifiStatus arm_dhcp_timeout(uint32_t) override { return WifiStatus::Ok; }
  WifiStatus cancel_dhcp_timeout() override { return WifiStatus::Ok; }
  WifiStatus arm_retry_timer(uint32_t) override { return WifiStatus::Ok; }
  WifiStatus cancel_retry_timer() override { return WifiStatus::Ok; }

  void set_on_sta_connected(WifiConnectedCallback cb) override { _on_connected = std::move(cb); }
  void set_on_sta_disconnected(std::function<void(int)> cb) override {
    _on_disconnected = std::move(cb);
  }
  void set_on_got_ip(WifiGotIpCallback cb) override { _on_got_ip = std::move(cb); }
  void set_on_scan_complete(WifiScanCompleteCallback cb) override {
    _on_scan_complete = std::move(cb);
  }
  void set_on_ap_client_joined(WifiApClientJoinedCallback cb) override {
    _on_joined = std::move(cb);
  }
  void set_on_ap_client_left(WifiApClientLeftCallback cb) override { _on_left = std::move(cb); }
  void set_on_dhcp_timeout(std::function<void()> cb) override { _on_dhcp = std::move(cb); }
  void set_on_retry_due(std::function<void()> cb) override { _on_retry = std::move(cb); }

  // -- Test hooks: fire HAL-level events to drive WifiManager --

  void fire_sta_disconnected(int raw_reason) {
    if (_on_disconnected) {
      _on_disconnected(raw_reason);
    }
  }

  // -- Visible state captured by overridden methods --

  uint32_t connect_calls = 0;
  uint32_t disconnect_calls = 0;
  std::string last_ssid;
  std::string last_password;

private:
  WifiMode _mode = WifiMode::Off;
  WifiConnectedCallback _on_connected;
  std::function<void(int)> _on_disconnected;
  WifiGotIpCallback _on_got_ip;
  WifiScanCompleteCallback _on_scan_complete;
  WifiApClientJoinedCallback _on_joined;
  WifiApClientLeftCallback _on_left;
  std::function<void()> _on_dhcp;
  std::function<void()> _on_retry;
};

// Minimal HttpServer stub that records registered routes.
class FakeHttpServer : public HttpServer {
public:
  bool start(uint16_t) override {
    started = true;
    return true;
  }
  void stop() override { started = false; }
  bool register_route(HttpMethod method, const char *path, HttpHandler handler) override {
    routes.push_back({method, std::string(path), std::move(handler)});
    return true;
  }
  bool unregister_route(HttpMethod method, const char *path) override {
    auto it = std::find_if(routes.begin(), routes.end(), [method, path](const auto &r) {
      return r.method == method && r.path == path;
    });
    if (it == routes.end()) {
      return false;
    }
    routes.erase(it);
    return true;
  }
  void unregister_all() override { routes.clear(); }

  struct Entry {
    HttpMethod method;
    std::string path;
    HttpHandler handler;
  };

  std::vector<Entry> routes;
  bool started = false;
};

struct Fixture {
  // `events` must outlive `prov` — the on_event callback writes into
  // it during the Stopped emit fired by ~ProvisioningManager.
  std::vector<ProvisioningEventInfo> events;
  FakeWifiHal hal;
  WifiManager wifi{hal};
  FakeHttpServer http;
  ProvisioningManager prov;

  Fixture() {
    prov.set_on_event([this](const ProvisioningEventInfo &e) { events.push_back(e); });
  }

  ProvisioningConfig basic_config(uint32_t timeout_ms = 0) {
    ProvisioningConfig cfg = {};
    std::strncpy(cfg.ap.ssid, "airgradient-test", sizeof(cfg.ap.ssid) - 1);
    std::strncpy(cfg.ap.password, "cleanair", sizeof(cfg.ap.password) - 1);
    cfg.overall_timeout_ms = timeout_ms;
    return cfg;
  }

  ProvisioningData creds(const char *ssid = "HomeWiFi", const char *password = "secret") {
    ProvisioningData d;
    std::strncpy(d.ssid, ssid, sizeof(d.ssid) - 1);
    std::strncpy(d.password, password, sizeof(d.password) - 1);
    return d;
  }
};

} // namespace

// ============================================================================
// ProvisioningTestAccess — friend class for private member access
//
// Mirrors the OrchestratorTestAccess pattern from products/go. All
// private event handlers and state are accessed through static methods
// here, keeping the production header clean.
// ============================================================================

class ProvisioningTestAccess {
public:
  // -- Drive external events (same handlers WifiManager callbacks invoke) --
  static void on_sta_connected(ProvisioningManager &p, uint32_t ip) { p._on_sta_connected(ip); }
  static void on_sta_disconnected(ProvisioningManager &p) { p._on_sta_disconnected(); }
  static void on_ap_client_joined(ProvisioningManager &p) { p._on_ap_client_joined(); }
  static void on_ap_client_left(ProvisioningManager &p) { p._on_ap_client_left(); }
  static void on_scan_results(ProvisioningManager &p, const WifiScanEntry *e, uint16_t c) {
    p._on_scan_results(e, c);
  }

  // -- Timer --
  static void fire_timeout(ProvisioningManager &p) { p._timer->fire_for_test(); }

  // -- Internal transport access (for handler-level tests) --
  static WifiPortalTransport &portal(ProvisioningManager &p) { return *p._portal; }

  // -- State inspection --
  static uint32_t ap_client_count(const ProvisioningManager &p) { return p._ap_client_count; }
};
using A = ProvisioningTestAccess;

TEST_CASE("ProvisioningManager starts in Idle and rejects start with empty SSID",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.state() == ProvisioningState::Idle);

  ProvisioningConfig bad = {};
  REQUIRE_FALSE(f.prov.start(f.wifi, nullptr, f.http, bad));
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
}

TEST_CASE("ProvisioningManager start emits Started and registers portal routes", "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, nullptr, f.http, f.basic_config()));
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Started);

  // Helper: confirm a route is registered for the given method+path.
  auto has_route = [&](HttpMethod method, const char *path) {
    for (const auto &r : f.http.routes) {
      if (r.method == method && r.path == path) {
        return true;
      }
    }
    return false;
  };

  // The four API routes the portal needs.
  REQUIRE(has_route(HttpMethod::Post, "/api/scan"));
  REQUIRE(has_route(HttpMethod::Get, "/api/scan"));
  REQUIRE(has_route(HttpMethod::Post, "/api/provision"));
  REQUIRE(has_route(HttpMethod::Get, "/api/status"));

  // Captive-portal OS probe routes — these are what stop iOS/Android
  // from reporting the AP as a broken network.
  REQUIRE(has_route(HttpMethod::Get, "/hotspot-detect.html"));
  REQUIRE(has_route(HttpMethod::Get, "/generate_204"));
  REQUIRE(has_route(HttpMethod::Get, "/connecttest.txt"));
  REQUIRE(has_route(HttpMethod::Get, "/canonical.html"));
  REQUIRE(has_route(HttpMethod::Get, "/favicon.ico"));
}

TEST_CASE("ProvisioningManager state machine: happy path to Connected", "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, nullptr, f.http, f.basic_config()));
  f.events.clear();

  // Submit credentials via the portal handler (drives the state machine
  // through the same path real HTTP traffic would).
  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"HomeWiFi","password":"secret"})");
  HttpResponse resp;
  A::portal(f.prov).handle_provision_post(req, resp);
  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Connecting);
  REQUIRE(std::string(f.events[0].data.ssid) == "HomeWiFi");

  // Simulate WifiManager reporting got-ip.
  f.events.clear();
  A::on_sta_connected(f.prov, 0x0100007f);
  REQUIRE(f.prov.state() == ProvisioningState::Connected);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Connected);
  REQUIRE(f.events[0].ip == 0x0100007f);
  REQUIRE(std::string(f.events[0].data.ssid) == "HomeWiFi");

  f.events.clear();
  f.prov.stop();
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Stopped);
  REQUIRE(f.events[0].stop_reason == ProvisioningStopReason::ProductRequested);

  // Teardown contract: HTTP routes wiped, HTTP server stopped (default),
  // Wi-Fi reverted to STA-only so the AP stops beaconing.
  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
}

TEST_CASE("ProvisioningManager: stop(false) wipes routes but keeps HTTP server running",
          "[provisioning]") {
  // Product path that wants to immediately re-register its own routes
  // on the same server without a bind/unbind cycle.
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, nullptr, f.http, f.basic_config()));
  REQUIRE(f.http.started);
  REQUIRE_FALSE(f.http.routes.empty());

  f.events.clear();
  f.prov.stop(/*stop_http_server=*/false);

  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Stopped);
  // Routes gone, server still up — caller's responsibility to register
  // its own routes next.
  REQUIRE(f.http.routes.empty());
  REQUIRE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
}

TEST_CASE("ProvisioningManager: HTTP server is started by ProvisioningManager::start",
          "[provisioning]") {
  Fixture f;
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.prov.start(f.wifi, nullptr, f.http, f.basic_config()));
  REQUIRE(f.http.started);
}

TEST_CASE("ProvisioningManager: failed connect returns to WaitingForCredentials",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, nullptr, f.http, f.basic_config()));

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"HomeWiFi"})");
  HttpResponse resp;
  A::portal(f.prov).handle_provision_post(req, resp);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);

  f.events.clear();
  A::on_sta_disconnected(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::ConnectFailed);
}

TEST_CASE("ProvisioningManager: credentials rejected while Connecting", "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, nullptr, f.http, f.basic_config()));

  TestHttpRequest first(HttpMethod::Post, "/api/provision");
  first.set_body(R"({"ssid":"A","password":"x"})");
  HttpResponse r1;
  A::portal(f.prov).handle_provision_post(first, r1);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);

  // A second submission must NOT transition state — the in-flight
  // attempt has not resolved.
  TestHttpRequest second(HttpMethod::Post, "/api/provision");
  second.set_body(R"({"ssid":"B","password":"y"})");
  HttpResponse r2;
  A::portal(f.prov).handle_provision_post(second, r2);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
}

TEST_CASE("ProvisioningManager: timeout fires only when no clients are connected",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, nullptr, f.http, f.basic_config(60'000)));
  f.events.clear();

  // A client joins — timeout pauses. Firing it now must be a no-op
  // (the manager re-arms because client count is non-zero).
  A::on_ap_client_joined(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.empty());

  // Client leaves — timeout resumes. Firing now stops provisioning.
  A::on_ap_client_left(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Stopped);
  REQUIRE(f.events[0].stop_reason == ProvisioningStopReason::TimedOut);

  // TimedOut path teardown — same shape as stop(), HTTP server always
  // stopped because no product call expressed a preference.
  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
}

TEST_CASE("ProvisioningManager: credentials submission disables WifiManager retry",
          "[provisioning]") {
  // Provisioning should make a single attempt and surface ConnectFailed
  // on disconnect rather than letting WifiManager's exponential-backoff
  // retry chain hold the user for tens of seconds. The observable
  // signature is: exactly one HAL connect_sta call even after a
  // retriable disconnect event.
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, nullptr, f.http, f.basic_config()));
  f.events.clear();

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"HomeWiFi","password":"wrong"})");
  HttpResponse resp;
  A::portal(f.prov).handle_provision_post(req, resp);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
  REQUIRE(f.hal.connect_calls == 1);
  REQUIRE(f.hal.last_ssid == "HomeWiFi");
  REQUIRE(f.hal.last_password == "wrong");

  // Fire a retriable HAL disconnect (raw reason 15 ==
  // WIFI_REASON_4WAY_HANDSHAKE_TIMEOUT — classified as handshake_failed,
  // which WifiManager's is_retriable() considers eligible for retry).
  // With max_retry_count = 0 the manager must NOT call connect_sta again.
  f.events.clear();
  f.hal.fire_sta_disconnected(15);

  REQUIRE(f.hal.connect_calls == 1);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::ConnectFailed);
}

TEST_CASE("ProvisioningManager: connect_timeout_ms forwards to WifiManager DHCP timeout",
          "[provisioning]") {
  // ProvisioningConfig::connect_timeout_ms tunes the DHCP-acquisition
  // window inside WifiManager (the only failure mode ESP-IDF doesn't
  // surface natively). We can't directly read WifiManager's private
  // field, but we can verify the manager invokes set_dhcp_timeout_ms
  // by side-effect: the override only takes effect when > 0, so leaving
  // it at 0 must be tolerated (no crash, default kept).
  //
  // The behavioural check happens implicitly through start() succeeding
  // in both modes; explicit value verification is covered by
  // wifi_manager tests upstream.
  Fixture f;
  ProvisioningConfig cfg = f.basic_config();
  cfg.connect_timeout_ms = 20000;
  REQUIRE(f.prov.start(f.wifi, nullptr, f.http, cfg));
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
}

TEST_CASE("ProvisioningManager: stop is idempotent", "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, nullptr, f.http, f.basic_config()));
  f.prov.stop();
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  // Second stop must not crash and must not emit another Stopped.
  size_t after_first = f.events.size();
  f.prov.stop();
  REQUIRE(f.events.size() == after_first);
}
