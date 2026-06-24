/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_ACTION_HANDLER_H
#define AG_LOCAL_SERVER_ACTION_HANDLER_H

#include "types/local_server_result.h"

// Product-supplied dispatcher for POST /api/v1/actions/<id>.
//
// Actions are fire-and-forget commands. trigger() must not block: it
// dispatches the work (for example queues a CO2 calibration on the product's
// worker) and returns immediately. No progress is reported; a consumer
// observes the effect indirectly (for example the CO2 reading settling after
// calibration).
class ActionHandler {
public:
  virtual ~ActionHandler() = default;

  // Dispatch a named action. The component maps the result to a status:
  // Dispatched -> 200, Rejected -> 403, NotSupported -> 404.
  virtual ActionResult trigger(ActionId action) = 0;
};

#endif // AG_LOCAL_SERVER_ACTION_HANDLER_H
