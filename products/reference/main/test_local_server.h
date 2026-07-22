#ifndef TEST_LOCAL_SERVER_H
#define TEST_LOCAL_SERVER_H

// Connects to a Wi-Fi router as a station, starts an IdfHttpServer on port
// 80, and registers the airgradient-local-server v1 API on it with demo
// providers (synthetic measures, an in-memory config, and a logging action
// handler). Blocks forever so the API stays available for manual probing.
//
// Set the network credentials at build time by passing them through
// EXTRA_CXXFLAGS (defaults: SSID "airgradient", password "cleanair"):
//   -DTEST_LOCAL_SERVER_WIFI_SSID="my-ssid"
//   -DTEST_LOCAL_SERVER_WIFI_PASSWORD="my-pass"
//
// Then watch the serial log for the leased IP and probe it from any host on
// the same network:
//   GET  http://<device-ip>/api/v1/measures
//   GET  http://<device-ip>/api/v1/config
//   PUT  http://<device-ip>/api/v1/config            (partial JSON -> 202)
//   POST http://<device-ip>/api/v1/actions/calibrate-co2  (-> 200)
//   POST http://<device-ip>/api/v1/actions/test-leds      (-> 200)
//
// A 202 confirms admission, not completion. Poll GET /api/v1/config for
// convergence; retry a structured 503 busy response using client-owned timing.
void run_test_local_server();

#endif // TEST_LOCAL_SERVER_H
