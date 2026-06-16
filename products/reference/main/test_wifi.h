#ifndef TEST_WIFI_H
#define TEST_WIFI_H

// Exercises airgradient-wifi (EspWifiHal + WifiManager) on real hardware.
//
// STA mode (default): triggers a scan, logs results, then connects to the
// SSID / password supplied via the TEST_WIFI_SSID / TEST_WIFI_PASSWORD
// macros and keeps printing status_snapshot() every few seconds. The
// configured retry / DHCP-timeout / mDNS lifecycle is exercised end to
// end; transient disconnects auto-retry per the spec, non-retriable
// reasons (auth failure, no AP, DHCP timeout) trigger the on_disconnected
// callback.
//
// Multi-SSID mode: define TEST_WIFI_MULTI_SSID to seed the saved-network
// store (TEST_WIFI_SSID[/_2]) and run an empty-SSID auto-connect — scan,
// intersect with saved networks, rank by RSSI, single-attempt failover.
// ESP-IDF's own Wi-Fi NVS stays empty (RAM storage).
//
// AP mode: define TEST_WIFI_RUN_AP to start a soft-AP using
// TEST_WIFI_AP_SSID / TEST_WIFI_AP_PASSWORD instead. Logs join / leave
// events for clients.
//
// Provide credentials at build time, e.g.:
//   idf.py -C products/reference -DEXTRA_CXXFLAGS='-DRUN_TEST_WIFI
//   -DTEST_WIFI_SSID="MyNet" -DTEST_WIFI_PASSWORD="secret"' build
//
// Multi-SSID example:
//   idf.py -C products/reference -DEXTRA_CXXFLAGS='-DRUN_TEST_WIFI
//   -DTEST_WIFI_MULTI_SSID -DTEST_WIFI_SSID="Home" -DTEST_WIFI_PASSWORD="p1"
//   -DTEST_WIFI_SSID_2="Office" -DTEST_WIFI_PASSWORD_2="p2"' build
void run_test_wifi();

#endif // TEST_WIFI_H
