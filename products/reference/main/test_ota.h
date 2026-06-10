#ifndef TEST_OTA_H
#define TEST_OTA_H

// Exercises airgradient-ota (WifiHttpOtaSource + EspOtaImageWriter driven by
// OtaUpdater) on real hardware.
//
// Connects WiFi STA using the WIFI_SSID / WIFI_PASSWORD constants at the top
// of test_ota.cpp, then runs OtaUpdater::run() for a set of cases and logs the
// progress callbacks and the final OtaStatus for each.
//
// By default only NON-APPLYING cases run (an up-to-date probe and a guaranteed
// DNS-failure), so the smoke test never rewrites the boot partition. To
// exercise a real download-and-apply, build with TEST_OTA_ALLOW_APPLY defined;
// on a successful Ok the new image is staged but the test does NOT reboot
// (per spec, the product decides). Reboot manually to run it.
//
// Provide credentials / target at build time or by editing the constants, e.g.:
//   idf.py -C products/reference -DEXTRA_CXXFLAGS='-DRUN_TEST_OTA
//   -DTEST_OTA_ALLOW_APPLY' build
void run_test_ota();

#endif // TEST_OTA_H
