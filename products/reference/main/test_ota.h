#ifndef TEST_OTA_H
#define TEST_OTA_H

// Exercises airgradient-ota on real hardware. The transport is selected at
// build time:
//
//   default (no macro) -> WiFi PULL path
//     WifiHttpOtaSource + EspOtaImageWriter driven by OtaUpdater. Connects WiFi
//     STA using the WIFI_SSID / WIFI_PASSWORD constants at the top of
//     test_ota.cpp, then runs OtaUpdater::run() for a set of cases and logs the
//     progress callbacks and the final OtaStatus for each. By default only
//     NON-APPLYING cases run (an up-to-date probe and a guaranteed DNS-failure),
//     so it never rewrites the boot partition. Build with TEST_OTA_ALLOW_APPLY
//     to add a real download-and-apply case; on Ok the image is staged but the
//     test does NOT reboot.
//
//   -DTEST_OTA_BLE -> BLE PUSH path
//     OtaBleService on a borrowed NimbleBleServer (authenticated pairing).
//     Attaches the OTA GATT service before advertising, then advertises and
//     waits for a phone to push an image, logging every lifecycle transition.
//     On a successful Done the image is staged but the test does NOT reboot
//     (per spec, the product decides). Requires CONFIG_BT_NIMBLE_ENABLED.
//
// Select RUN_TEST_OTA in main.cpp (the default), then choose the transport via
// the EXTRA_CXXFLAGS ENVIRONMENT variable, which ESP-IDF appends to the target
// compile options (note: it is an env var, not an idf.py -D cache variable):
//   EXTRA_CXXFLAGS='-DTEST_OTA_BLE' idf.py -C products/reference build
//   EXTRA_CXXFLAGS='-DTEST_OTA_ALLOW_APPLY' idf.py -C products/reference build
// Run `idf.py -C products/reference reconfigure` after changing EXTRA_CXXFLAGS.
void run_test_ota();

#endif // TEST_OTA_H
