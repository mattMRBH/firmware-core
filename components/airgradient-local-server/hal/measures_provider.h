/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_MEASURES_PROVIDER_H
#define AG_LOCAL_SERVER_MEASURES_PROVIDER_H

#include "measures_types.h"
#include "types/system_info.h"

// Product-supplied source of live readings for GET /api/v1/measures.
//
// Ownership : product owns the implementation.
// Lifetime  : must outlive the LocalServer.
// Thread-safe: yes — called from the httpd task; return cached snapshots.
// Blocking  : should not block.
class MeasuresProvider {
public:
  virtual ~MeasuresProvider() = default;

  // Snapshot of current readings for GET /api/v1/measures.
  virtual Measures get_measures() = 0;

  // Identity + link info embedded in the measures payload.
  virtual SystemInfo get_system_info() = 0;
};

#endif // AG_LOCAL_SERVER_MEASURES_PROVIDER_H
