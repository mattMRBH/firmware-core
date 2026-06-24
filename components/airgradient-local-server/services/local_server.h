/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_SERVICES_LOCAL_SERVER_H
#define AG_LOCAL_SERVER_SERVICES_LOCAL_SERVER_H

#include <cstddef>

#include "hal/action_handler.h"
#include "hal/config_provider.h"
#include "hal/http_request.h"
#include "hal/http_response.h"
#include "hal/http_server.h"
#include "hal/measures_provider.h"
#include "types/api_error.h"
#include "types/http_types.h"
#include "types/local_server_result.h"

// Pull in Kconfig values when building with ESP-IDF; fall back to the spec
// default for native host-test builds where no sdkconfig.h exists.
#if defined(__has_include) && __has_include("sdkconfig.h")
#include "sdkconfig.h"
#endif

#ifndef CONFIG_AG_LOCAL_SERVER_JSON_BUF
#define CONFIG_AG_LOCAL_SERVER_JSON_BUF 3072
#endif

// Generic, product-agnostic local HTTP API (/api/v1/...) layered on
// HttpServer. The component owns versioned routing, the wire schema, JSON,
// and the structured error model; products supply live data and config
// semantics through the provider seams.
//
// Ownership : holds references only; the HttpServer and all providers MUST
//             outlive this object.
// Copy/move : non-copyable, non-movable (handlers capture `this`).
// Thread-safe: no (begin / end are setup-time calls from one task).
// Blocking  : no.
class LocalServer {
public:
  struct Providers {
    MeasuresProvider &measures;                          // required
    ConfigProvider *config = nullptr;                    // optional
    ConfigAccess config_access = ConfigAccess::Disabled; // GET and/or PUT
    ActionHandler *actions = nullptr;                    // optional
  };

  LocalServer(HttpServer &server, const Providers &providers);

  // RAII: unregisters any routes still registered, so no captured-`this`
  // handler can outlive the object on the server.
  ~LocalServer();

  LocalServer(const LocalServer &) = delete;
  LocalServer &operator=(const LocalServer &) = delete;
  LocalServer(LocalServer &&) = delete;
  LocalServer &operator=(LocalServer &&) = delete;

  // Register the versioned routes for the providers that are present:
  //   measures            -> GET  /api/v1/measures
  //   config ReadOnly     -> GET  /api/v1/config
  //   config ReadWrite    -> GET + PUT /api/v1/config
  //   actions present     -> POST /api/v1/actions/<id> for EVERY ActionId in
  //                          the catalog, regardless of model support
  //                          (unsupported -> structured 404 at request time)
  //
  // Idempotent: a no-op returning true if already begun. Transactional: if
  // any route fails to register, the routes registered so far in this call
  // are rolled back (unregistered) and begin() returns false. May be called
  // lazily (for example when the device joins a network).
  bool begin();

  // Unregister ONLY the routes this LocalServer registered (tracked in
  // _routes). Never calls HttpServer::unregister_all(), so product- or
  // provisioning-owned routes on the same server are untouched. Safe to call
  // when not begun.
  void end();

private:
  struct OwnedRoute {
    HttpMethod method;
    const char *path; // static-lifetime literal
  };

  bool _register(HttpMethod method, const char *path, HttpHandler handler);
  void _send_error(HttpResponse &resp, ApiErrorCode code, const char *field);

  void _handle_get_measures(const HttpRequest &, HttpResponse &);
  void _handle_get_config(const HttpRequest &, HttpResponse &);
  void _handle_put_config(const HttpRequest &, HttpResponse &);
  void _handle_action(ActionId action, const HttpRequest &, HttpResponse &);

  HttpServer &_server;
  MeasuresProvider &_measures;
  ConfigProvider *_config;
  ConfigAccess _config_access;
  ActionHandler *_actions;

  static constexpr size_t MAX_OWNED_ROUTES = 5; // measures + config x2 + 2 actions
  OwnedRoute _routes[MAX_OWNED_ROUTES] = {};
  size_t _route_count = 0;
  bool _begun = false;

  char _json_buf[CONFIG_AG_LOCAL_SERVER_JSON_BUF] = {};
};

#endif // AG_LOCAL_SERVER_SERVICES_LOCAL_SERVER_H
