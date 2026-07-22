/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_TESTS_FAKE_PROVIDERS_H
#define AG_LOCAL_SERVER_TESTS_FAKE_PROVIDERS_H

#include <algorithm>
#include <cstdint>
#include <string>
#include <utility>
#include <vector>

#include "hal/action_handler.h"
#include "hal/config_provider.h"
#include "hal/http_request.h"
#include "hal/http_response.h"
#include "hal/http_server.h"
#include "hal/measures_provider.h"
#include "types/http_types.h"

// Recording HttpServer for handler / lifecycle tests. Records every
// registered route and lets a test look up and invoke a handler by
// method + path. Set `fail_path` to force register_route to reject a
// specific path (transactional begin() rollback test).
class RecordingHttpServer : public HttpServer {
public:
  struct Route {
    HttpMethod method;
    std::string path;
    HttpHandler handler;
  };

  std::vector<Route> routes;
  std::string fail_path; // register_route returns false for this path

  bool start(uint16_t) override { return true; }
  void stop() override {}

  bool register_route(HttpMethod method, const char *path, HttpHandler handler) override {
    const std::string p = (path == nullptr) ? "" : path;
    if (!fail_path.empty() && p == fail_path) {
      return false;
    }
    routes.push_back({method, p, std::move(handler)});
    return true;
  }

  bool unregister_route(HttpMethod method, const char *path) override {
    const std::string p = (path == nullptr) ? "" : path;
    auto it = std::find_if(routes.begin(), routes.end(),
                           [&](const Route &r) { return r.method == method && r.path == p; });
    if (it == routes.end()) {
      return false;
    }
    routes.erase(it);
    return true;
  }

  void unregister_all() override { routes.clear(); }

  bool has_route(HttpMethod method, const char *path) const {
    const std::string p = (path == nullptr) ? "" : path;
    return std::any_of(routes.begin(), routes.end(),
                       [&](const Route &r) { return r.method == method && r.path == p; });
  }

  // Invoke the handler registered for method + path. Returns false if no
  // matching route exists.
  bool invoke(HttpMethod method, const char *path, const HttpRequest &req,
              HttpResponse &resp) const {
    const std::string p = (path == nullptr) ? "" : path;
    auto it = std::find_if(routes.begin(), routes.end(),
                           [&](const Route &r) { return r.method == method && r.path == p; });
    if (it == routes.end()) {
      return false;
    }
    it->handler(req, resp);
    return true;
  }
};

class FakeMeasuresProvider : public MeasuresProvider {
public:
  Measures measures;
  SystemInfo info;

  Measures get_measures() override { return measures; }
  SystemInfo get_system_info() override { return info; }
};

class FakeConfigProvider : public ConfigProvider {
public:
  LocalServerConfig config; // returned by get_config()
  ConfigSubmitResult submit_result{ConfigSubmitStatus::Accepted,
                                   ConfigFieldId::None}; // returned by submit
  LocalServerConfig last_submitted;
  bool submit_called = false;

  LocalServerConfig get_config() override { return config; }

  ConfigSubmitResult submit_config(const LocalServerConfig &partial) override {
    submit_called = true;
    last_submitted = partial;
    return submit_result;
  }
};

class FakeActionHandler : public ActionHandler {
public:
  ActionResult result_calibrate{ActionStatus::Dispatched};
  ActionResult result_test_leds{ActionStatus::Dispatched};
  ActionId last_action = ActionId::CalibrateCo2;
  bool triggered = false;

  ActionResult trigger(ActionId action) override {
    triggered = true;
    last_action = action;
    return action == ActionId::CalibrateCo2 ? result_calibrate : result_test_leds;
  }
};

#endif // AG_LOCAL_SERVER_TESTS_FAKE_PROVIDERS_H
