/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "../services/wifi_credential_store.h"
#include "fake_config_store.h"

namespace {

// Convenience: list SSIDs newest-first into a vector of strings.
std::vector<std::string> list_ssids(const WifiCredentialStore &store) {
  char out[WIFI_MAX_SAVED_NETWORKS][33] = {};
  const uint8_t count = store.list(out, WIFI_MAX_SAVED_NETWORKS);
  std::vector<std::string> result;
  for (uint8_t i = 0; i < count; ++i) {
    result.emplace_back(out[i]);
  }
  return result;
}

std::string password_for(const WifiCredentialStore &store, const char *ssid) {
  WifiCredential all[WIFI_MAX_SAVED_NETWORKS];
  const uint8_t count = store.load_all(all, WIFI_MAX_SAVED_NETWORKS);
  for (uint8_t i = 0; i < count; ++i) {
    if (std::strcmp(all[i].ssid, ssid) == 0) {
      return all[i].password;
    }
  }
  return "<none>";
}

} // namespace

// ---------------------------------------------------------------------------
// Basic CRUD
// ---------------------------------------------------------------------------

TEST_CASE("add inserts newest-first", "[wifi-creds][add]") {
  FakeConfigStore backend;
  WifiCredentialStore store(&backend);

  REQUIRE(store.add("Net1", "pass1") == WifiStatus::Ok);
  REQUIRE(store.add("Net2", "pass2") == WifiStatus::Ok);
  REQUIRE(store.add("Net3", "pass3") == WifiStatus::Ok);

  REQUIRE(list_ssids(store) == std::vector<std::string>{"Net3", "Net2", "Net1"});
  REQUIRE(store.has_networks());
}

TEST_CASE("re-adding an existing SSID refreshes password and marks newest", "[wifi-creds][add]") {
  FakeConfigStore backend;
  WifiCredentialStore store(&backend);

  store.add("Net1", "pass1");
  store.add("Net2", "pass2");
  REQUIRE(store.add("Net1", "newpass") == WifiStatus::Ok);

  REQUIRE(list_ssids(store) == std::vector<std::string>{"Net1", "Net2"});
  REQUIRE(password_for(store, "Net1") == "newpass");
}

TEST_CASE("overflow evicts the oldest entry", "[wifi-creds][add]") {
  FakeConfigStore backend;
  WifiCredentialStore store(&backend);

  store.add("Net1", "p1");
  store.add("Net2", "p2");
  store.add("Net3", "p3");
  store.add("Net4", "p4"); // evicts Net1 (oldest)

  REQUIRE(list_ssids(store) == std::vector<std::string>{"Net4", "Net3", "Net2"});
}

TEST_CASE("remove drops the entry and compacts", "[wifi-creds][remove]") {
  FakeConfigStore backend;
  WifiCredentialStore store(&backend);

  store.add("Net1", "p1");
  store.add("Net2", "p2");
  store.add("Net3", "p3");

  REQUIRE(store.remove("Net2") == WifiStatus::Ok);
  REQUIRE(list_ssids(store) == std::vector<std::string>{"Net3", "Net1"});
}

TEST_CASE("clear empties the store", "[wifi-creds][clear]") {
  FakeConfigStore backend;
  WifiCredentialStore store(&backend);

  store.add("Net1", "p1");
  store.add("Net2", "p2");
  REQUIRE(store.clear() == WifiStatus::Ok);

  REQUIRE_FALSE(store.has_networks());
  REQUIRE(list_ssids(store).empty());
}

TEST_CASE("empty password (open network) is allowed", "[wifi-creds][add]") {
  FakeConfigStore backend;
  WifiCredentialStore store(&backend);

  REQUIRE(store.add("OpenNet", "") == WifiStatus::Ok);
  REQUIRE(store.add("NullPw", nullptr) == WifiStatus::Ok);
  REQUIRE(password_for(store, "OpenNet").empty());
  REQUIRE(password_for(store, "NullPw").empty());
}

// ---------------------------------------------------------------------------
// API validation
// ---------------------------------------------------------------------------

TEST_CASE("add rejects null/empty/overlong SSID", "[wifi-creds][validation]") {
  FakeConfigStore backend;
  WifiCredentialStore store(&backend);

  REQUIRE(store.add(nullptr, "p") == WifiStatus::InvalidArgument);
  REQUIRE(store.add("", "p") == WifiStatus::InvalidArgument);
  const std::string overlong_ssid(33, 'a'); // > 32
  REQUIRE(store.add(overlong_ssid.c_str(), "p") == WifiStatus::InvalidArgument);
}

TEST_CASE("add rejects overlong password but accepts the max length", "[wifi-creds][validation]") {
  FakeConfigStore backend;
  WifiCredentialStore store(&backend);

  const std::string overlong_pw(64, 'x'); // > 63
  REQUIRE(store.add("Net", overlong_pw.c_str()) == WifiStatus::InvalidArgument);

  const std::string max_pw(63, 'x');
  REQUIRE(store.add("Net", max_pw.c_str()) == WifiStatus::Ok);

  const std::string max_ssid(32, 'a');
  REQUIRE(store.add(max_ssid.c_str(), "p") == WifiStatus::Ok);
}

TEST_CASE("remove validation: invalid arg / not found / ok", "[wifi-creds][validation]") {
  FakeConfigStore backend;
  WifiCredentialStore store(&backend);

  REQUIRE(store.remove(nullptr) == WifiStatus::InvalidArgument);
  REQUIRE(store.remove("") == WifiStatus::InvalidArgument);
  REQUIRE(store.remove("Missing") == WifiStatus::NotFound);

  store.add("Net", "p");
  REQUIRE(store.remove("Net") == WifiStatus::Ok);
}

// ---------------------------------------------------------------------------
// No-store behavior
// ---------------------------------------------------------------------------

TEST_CASE("no store: methods surface failure, never silent success", "[wifi-creds][no-store]") {
  WifiCredentialStore store(nullptr);

  REQUIRE_FALSE(store.has_networks());
  char out[WIFI_MAX_SAVED_NETWORKS][33] = {};
  REQUIRE(store.list(out, WIFI_MAX_SAVED_NETWORKS) == 0);
  REQUIRE(store.add("Net", "p") == WifiStatus::Failed);
  REQUIRE(store.remove("Net") == WifiStatus::Failed);
  REQUIRE(store.clear() == WifiStatus::Failed);
}

// ---------------------------------------------------------------------------
// Load resilience / self-heal
// ---------------------------------------------------------------------------

TEST_CASE("missing count key reads as empty", "[wifi-creds][load]") {
  FakeConfigStore backend; // no count key written
  WifiCredentialStore store(&backend);

  REQUIRE_FALSE(store.has_networks());
  REQUIRE(list_ssids(store).empty());
}

TEST_CASE("out-of-range count is clamped and self-healed", "[wifi-creds][load]") {
  FakeConfigStore backend;
  backend.set_int("count", 99);
  backend.set_string("ssid0", "Net1");
  backend.set_string("pw0", "p1");
  WifiCredentialStore store(&backend);

  REQUIRE(list_ssids(store) == std::vector<std::string>{"Net1"});
  // Self-heal: the normalised count is rewritten back.
  int healed = 0;
  REQUIRE(backend.get_int("count", healed) == ConfigStoreResult::OK);
  REQUIRE(healed == 1);
}

TEST_CASE("missing slot keys are skipped and compacted newest-first", "[wifi-creds][load]") {
  FakeConfigStore backend;
  backend.set_int("count", 3);
  backend.set_string("ssid0", "Net0");
  backend.set_string("pw0", "p0");
  // slot 1 missing entirely
  backend.set_string("ssid2", "Net2");
  backend.set_string("pw2", "p2");
  WifiCredentialStore store(&backend);

  REQUIRE(list_ssids(store) == std::vector<std::string>{"Net0", "Net2"});
  int healed = 0;
  REQUIRE(backend.get_int("count", healed) == ConfigStoreResult::OK);
  REQUIRE(healed == 2);
}

TEST_CASE("empty SSID slot is skipped", "[wifi-creds][load]") {
  FakeConfigStore backend;
  backend.set_int("count", 2);
  backend.set_string("ssid0", ""); // empty SSID => skip
  backend.set_string("pw0", "p0");
  backend.set_string("ssid1", "Net1");
  backend.set_string("pw1", "p1");
  WifiCredentialStore store(&backend);

  REQUIRE(list_ssids(store) == std::vector<std::string>{"Net1"});
}

TEST_CASE("missing password key skips the entry", "[wifi-creds][load]") {
  FakeConfigStore backend;
  backend.set_int("count", 1);
  backend.set_string("ssid0", "Net0"); // pw0 absent
  WifiCredentialStore store(&backend);

  REQUIRE(list_ssids(store).empty());
}

TEST_CASE("overlong stored strings are truncated defensively", "[wifi-creds][load]") {
  FakeConfigStore backend;
  backend.set_int("count", 1);
  backend.set_string("ssid0", std::string(40, 'a'));
  backend.set_string("pw0", std::string(80, 'b'));
  WifiCredentialStore store(&backend);

  const auto ssids = list_ssids(store);
  REQUIRE(ssids.size() == 1);
  REQUIRE(ssids[0].size() == 32);
  REQUIRE(password_for(store, ssids[0].c_str()).size() == 63);
}

TEST_CASE("self-heal does not rewrite a clean store", "[wifi-creds][load]") {
  FakeConfigStore backend;
  WifiCredentialStore store(&backend);
  store.add("Net1", "p1"); // writes once (commit_count == 1)
  const int before = backend.commit_count;

  // A clean load must not trigger a rewrite.
  (void)store.has_networks();
  (void)list_ssids(store);
  REQUIRE(backend.commit_count == before);
}
