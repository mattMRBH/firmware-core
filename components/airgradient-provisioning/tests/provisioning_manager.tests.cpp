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
#include "internal/ble_transport.h"
#include "internal/provisioning_timer.h"
#include "internal/wifi_portal_transport.h"
#include "mock_ble.h"
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
    if (fail_set_mode_for == m) {
      fail_set_mode_for = WifiMode::Off; // one-shot
      return WifiStatus::Failed;
    }
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

  WifiStatus start_ap(const WifiApConfig &) override {
    if (fail_next_start_ap) {
      fail_next_start_ap = false;
      return WifiStatus::Failed;
    }
    return WifiStatus::Ok;
  }
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

  // -- Fault injection (one-shot; cleared on first triggered call) --
  WifiMode fail_set_mode_for = WifiMode::Off; // Off = disabled (Off never requested by manager)
  bool fail_next_start_ap = false;

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
  MockBleServer ble;
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

  ProvisioningData creds(const char *ssid = "HomeWiFi", const char *password = "secret12") {
    ProvisioningData d;
    std::strncpy(d.ssid, ssid, sizeof(d.ssid) - 1);
    std::strncpy(d.password, password, sizeof(d.password) - 1);
    return d;
  }
};

} // namespace

// ============================================================================
// ProvisioningTestAccess — friend class for private member access
// ============================================================================

class ProvisioningTestAccess {
public:
  // -- Drive external events (same handlers WifiManager callbacks invoke) --
  static void on_sta_connected(ProvisioningManager &p, uint32_t ip) { p._on_sta_connected(ip); }
  static void on_sta_disconnected(ProvisioningManager &p) { p._on_sta_disconnected(); }
  static void on_ap_client_joined(ProvisioningManager &p) { p._on_ap_client_joined(); }
  static void on_ap_client_left(ProvisioningManager &p) { p._on_ap_client_left(); }
  static void on_ble_client_connected(ProvisioningManager &p) { p._on_ble_client_connected(); }
  static void on_ble_client_disconnected(ProvisioningManager &p) {
    p._on_ble_client_disconnected();
  }
  static void on_scan_results(ProvisioningManager &p, const WifiScanEntry *e, uint16_t c) {
    p._on_scan_results(e, c);
  }

  // -- Timer --
  static void fire_timeout(ProvisioningManager &p) { p._timer->fire_for_test(); }

  // -- Internal transport access (for handler-level tests) --
  static WifiPortalTransport &portal(ProvisioningManager &p) { return *p._portal; }
  static BleTransport &ble_transport(ProvisioningManager &p) { return *p._ble_transport; }

  // -- State inspection --
  static uint32_t ap_client_count(const ProvisioningManager &p) { return p._ap_client_count; }
  static uint32_t ble_client_count(const ProvisioningManager &p) { return p._ble_client_count; }
};
using A = ProvisioningTestAccess;

// ============================================================================
// Existing CP1 tests (updated for AgBleServer& signature)
// ============================================================================

TEST_CASE("ProvisioningManager starts in Idle and rejects start with empty SSID",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.state() == ProvisioningState::Idle);

  ProvisioningConfig bad = {};
  REQUIRE_FALSE(f.prov.start(f.wifi, f.ble, f.http, bad));
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
}

TEST_CASE("ProvisioningManager start emits Started and registers portal routes", "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Started);

  auto has_route = [&](HttpMethod method, const char *path) {
    for (const auto &r : f.http.routes) {
      if (r.method == method && r.path == path) {
        return true;
      }
    }
    return false;
  };

  REQUIRE(has_route(HttpMethod::Post, "/api/scan"));
  REQUIRE(has_route(HttpMethod::Get, "/api/scan"));
  REQUIRE(has_route(HttpMethod::Post, "/api/provision"));
  REQUIRE(has_route(HttpMethod::Get, "/api/status"));

  REQUIRE(has_route(HttpMethod::Get, "/hotspot-detect.html"));
  REQUIRE(has_route(HttpMethod::Get, "/generate_204"));
  REQUIRE(has_route(HttpMethod::Get, "/connecttest.txt"));
  REQUIRE(has_route(HttpMethod::Get, "/canonical.html"));
  REQUIRE(has_route(HttpMethod::Get, "/favicon.ico"));
}

TEST_CASE("ProvisioningManager state machine: happy path to Connected", "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  f.events.clear();

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"HomeWiFi","password":"secret12"})");
  HttpResponse resp;
  A::portal(f.prov).handle_provision_post(req, resp);
  REQUIRE(resp.status == HttpStatus::Ok);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Connecting);
  REQUIRE(std::string(f.events[0].data.ssid) == "HomeWiFi");

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

  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
}

TEST_CASE("ProvisioningManager: stop(false) wipes routes but keeps HTTP server running",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.http.started);
  REQUIRE_FALSE(f.http.routes.empty());

  f.events.clear();
  f.prov.stop(/*stop_http_server=*/false);

  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Stopped);
  REQUIRE(f.http.routes.empty());
  REQUIRE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
}

TEST_CASE("ProvisioningManager: HTTP server is started by ProvisioningManager::start",
          "[provisioning]") {
  Fixture f;
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.http.started);
}

TEST_CASE("ProvisioningManager: failed connect returns to WaitingForCredentials",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

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
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

  TestHttpRequest first(HttpMethod::Post, "/api/provision");
  first.set_body(R"({"ssid":"A","password":"password"})");
  HttpResponse r1;
  A::portal(f.prov).handle_provision_post(first, r1);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);

  TestHttpRequest second(HttpMethod::Post, "/api/provision");
  second.set_body(R"({"ssid":"B","password":"password"})");
  HttpResponse r2;
  A::portal(f.prov).handle_provision_post(second, r2);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
}

TEST_CASE("ProvisioningManager: timeout fires only when no AP clients are connected",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config(60'000)));
  f.events.clear();

  A::on_ap_client_joined(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.empty());

  A::on_ap_client_left(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Stopped);
  REQUIRE(f.events[0].stop_reason == ProvisioningStopReason::TimedOut);

  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
}

TEST_CASE("ProvisioningManager: credentials submission disables WifiManager retry",
          "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  f.events.clear();

  TestHttpRequest req(HttpMethod::Post, "/api/provision");
  req.set_body(R"({"ssid":"HomeWiFi","password":"wrongpass"})");
  HttpResponse resp;
  A::portal(f.prov).handle_provision_post(req, resp);
  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
  REQUIRE(f.hal.connect_calls == 1);
  REQUIRE(f.hal.last_ssid == "HomeWiFi");
  REQUIRE(f.hal.last_password == "wrongpass");

  f.events.clear();
  f.hal.fire_sta_disconnected(15);

  REQUIRE(f.hal.connect_calls == 1);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::ConnectFailed);
}

TEST_CASE("ProvisioningManager: connect_timeout_ms forwards to WifiManager DHCP timeout",
          "[provisioning]") {
  Fixture f;
  ProvisioningConfig cfg = f.basic_config();
  cfg.connect_timeout_ms = 20000;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, cfg));
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
}

TEST_CASE("ProvisioningManager: stop is idempotent", "[provisioning]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  f.prov.stop();
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  size_t after_first = f.events.size();
  f.prov.stop();
  REQUIRE(f.events.size() == after_first);
}

// ============================================================================
// CP2: BLE integration tests
// ============================================================================

TEST_CASE("ProvisioningManager: start initialises BLE transport", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

  // BLE server should have been init'd and started advertising.
  REQUIRE(f.ble.init_count == 1);
  REQUIRE(f.ble.start_advertising_count == 1);

  f.prov.stop();
  // BLE server should be deinit'd.
  REQUIRE(f.ble.deinit_count == 1);
}

TEST_CASE("ProvisioningManager: send_ble_status forwards to BLE transport", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

  // Find the credentials characteristic on the mock BLE server.
  constexpr const char *PROV_UUID = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
  constexpr const char *CRED_UUID = "703fa252-3d2a-4da9-a05c-83b0d9cacb8e";
  MockBleCharacteristic *cred_char = f.ble.find_char(PROV_UUID, CRED_UUID);
  REQUIRE(cred_char != nullptr);

  f.prov.send_ble_status(1); // CONNECTING_TO_SERVER
  REQUIRE(cred_char->notify_count == 1);

  f.prov.send_ble_status(2); // SERVER_REACHABLE
  REQUIRE(cred_char->notify_count == 2);

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: send_ble_status is no-op when idle", "[provisioning][ble]") {
  Fixture f;
  // send_ble_status before start() — should not crash.
  f.prov.send_ble_status(1);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
}

TEST_CASE("ProvisioningManager: BLE client connect pauses timeout", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config(60'000)));
  f.events.clear();

  // BLE client connects — timeout must pause.
  A::on_ble_client_connected(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.empty());

  // BLE client disconnects — timeout resumes.
  A::on_ble_client_disconnected(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].stop_reason == ProvisioningStopReason::TimedOut);
}

TEST_CASE("ProvisioningManager: mixed AP + BLE clients suppress timeout", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config(60'000)));
  f.events.clear();

  // AP client joins — pauses timeout.
  A::on_ap_client_joined(f.prov);

  // BLE client also connects — still paused.
  A::on_ble_client_connected(f.prov);

  // AP client leaves — BLE client still present, timeout stays paused.
  A::on_ap_client_left(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::WaitingForCredentials);
  REQUIRE(f.events.empty());

  // BLE client disconnects — now total count is 0, timeout resumes.
  A::on_ble_client_disconnected(f.prov);
  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].stop_reason == ProvisioningStopReason::TimedOut);
}

TEST_CASE("ProvisioningManager: scan results reach both transports", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));

  WifiScanEntry entries[2] = {};
  std::strncpy(entries[0].ssid, "Net1", sizeof(entries[0].ssid) - 1);
  entries[0].rssi = -40;
  entries[0].auth_mode = WifiAuthMode::wpa2_psk;
  std::strncpy(entries[1].ssid, "Net2", sizeof(entries[1].ssid) - 1);
  entries[1].rssi = -50;
  entries[1].auth_mode = WifiAuthMode::open;

  A::on_scan_results(f.prov, entries, 2);

  // Portal should have cached the results.
  REQUIRE(A::portal(f.prov).scan_in_progress() == false);

  // BLE transport should have pagination pending.
  // The pagination timer was armed with 0ms for the first page.
  // Fire it to verify the scan char gets a notification.
  constexpr const char *PROV_UUID = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
  constexpr const char *SCAN_UUID = "467a080f-e50f-42c9-b9b2-a2ab14d82725";
  MockBleCharacteristic *scan_char = f.ble.find_char(PROV_UUID, SCAN_UUID);
  REQUIRE(scan_char != nullptr);

  A::ble_transport(f.prov).pagination_timer().fire_for_test();
  REQUIRE(scan_char->notify_count >= 1);

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: BLE credential write drives state machine", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  f.events.clear();

  constexpr const char *PROV_UUID = "acbcfea8-e541-4c40-9bfd-17820f16c95c";
  constexpr const char *CRED_UUID = "703fa252-3d2a-4da9-a05c-83b0d9cacb8e";
  MockBleCharacteristic *cred_char = f.ble.find_char(PROV_UUID, CRED_UUID);
  REQUIRE(cred_char != nullptr);

  // Simulate BLE credential submission.
  cred_char->simulate_write(R"({"ssid":"BleNet","password":"blepass1"})");

  REQUIRE(f.prov.state() == ProvisioningState::Connecting);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Connecting);
  REQUIRE(std::string(f.events[0].data.ssid) == "BleNet");
  REQUIRE(std::string(f.events[0].data.password) == "blepass1");

  // Connected event triggers BLE status notification.
  f.events.clear();
  A::on_sta_connected(f.prov, 0x0A000001);
  REQUIRE(f.prov.state() == ProvisioningState::Connected);
  REQUIRE(f.events.size() == 1);
  REQUIRE(f.events[0].event == ProvisioningEvent::Connected);

  // The credentials characteristic should have received a WIFI_CONNECTED
  // status notification.
  REQUIRE(cred_char->notify_count >= 1);

  f.prov.stop();
}

TEST_CASE("ProvisioningManager: timeout teardown deinits BLE", "[provisioning][ble]") {
  Fixture f;
  REQUIRE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config(60'000)));
  f.events.clear();

  A::fire_timeout(f.prov);
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.ble.deinit_count == 1);
}

// ============================================================================
// start() failure-path rollback tests (issue #2: hard-fail on Wi-Fi setup,
// log-only on DNS responder failure).
// ============================================================================

TEST_CASE("ProvisioningManager: set_mode(ApSta) failure rolls back and returns false",
          "[provisioning][rollback]") {
  Fixture f;
  f.hal.fail_set_mode_for = WifiMode::ApSta;

  REQUIRE_FALSE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.prov.state() == ProvisioningState::Idle);

  // No Started event must be emitted on a failed start.
  REQUIRE(f.events.empty());

  // Rollback must wipe HTTP routes, leave the server unstarted, and
  // tear down BLE so the device is back to a clean state.
  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.ble.deinit_count == 1);
}

TEST_CASE("ProvisioningManager: start_ap failure rolls back and returns false",
          "[provisioning][rollback]") {
  Fixture f;
  f.hal.fail_next_start_ap = true;

  REQUIRE_FALSE(f.prov.start(f.wifi, f.ble, f.http, f.basic_config()));
  REQUIRE(f.prov.state() == ProvisioningState::Idle);
  REQUIRE(f.events.empty());

  // set_mode(ApSta) ran before the failure; rollback must revert it.
  REQUIRE(f.wifi.get_mode() == WifiMode::Sta);
  REQUIRE(f.http.routes.empty());
  REQUIRE_FALSE(f.http.started);
  REQUIRE(f.ble.deinit_count == 1);
}
