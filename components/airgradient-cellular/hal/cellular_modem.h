/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef CELLULAR_MODEM_H
#define CELLULAR_MODEM_H

#include <cstddef>

#include "../types/cellular_types.h"

// Execution context: task context only
// ISR-safe: no
// Thread-safe: no
// Blocking: yes
// Allocates: implementation-defined; the public API uses caller-owned buffers
// Ownership: implementations must not retain request or buffer pointers after
// the call returns
class CellularModem {
public:
  virtual ~CellularModem() = default;

  virtual CellularStatus init() = 0;
  virtual CellularStatus power_on() = 0;
  virtual CellularStatus power_off(bool force) = 0;
  virtual CellularStatus reset() = 0;
  virtual CellularStatus sleep() = 0;
  virtual CellularStatus reinitialize() = 0;

  virtual CellularStatus get_module_info(MutableStringBuffer out_module_info,
                                         size_t *out_length = nullptr) = 0;
  virtual CellularStatus get_sim_iccid(MutableStringBuffer out_iccid,
                                       size_t *out_length = nullptr) = 0;
  virtual CellularStatus check_sim_ready() = 0;
  virtual CellularStatus
  get_signal_quality(CellularSignalQuality *out_signal_quality) = 0;
  virtual CellularStatus get_ip_address(MutableStringBuffer out_ip_address,
                                        size_t *out_length = nullptr) = 0;

  virtual CellularStatus
  get_network_registration(CellularTechnology technology,
                           CellularNetworkRegistration *out_registration) = 0;
  virtual CellularStatus
  start_network_registration(const CellularRegistrationRequest &request) = 0;

  virtual CellularStatus resolve_dns(const char *hostname,
                                     MutableStringBuffer out_ip_address,
                                     size_t *out_length = nullptr) = 0;

  virtual CellularStatus http_get(const CellularHttpGetRequest &request,
                                  CellularHttpResponse *out_response) = 0;
  virtual CellularStatus http_post(const CellularHttpPostRequest &request,
                                   CellularHttpResponse *out_response) = 0;
};

#endif // CELLULAR_MODEM_H
