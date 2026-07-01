/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>

#include "../hal/cellular_modem.h"
#include "../types/cellular_types.h"

TEST_CASE("airgradient-cellular scaffold is present", "[airgradient-cellular]") { SUCCEED(); }

TEST_CASE("airgradient-cellular public types expose invalid sentinels", "[airgradient-cellular]") {
  const CellularSignalQuality signal_quality;
  const CellularHttpResponse response;

  REQUIRE(signal_quality.csq == CELLULAR_SIGNAL_CSQ_INVALID);
  REQUIRE(signal_quality.rssi_dbm == CELLULAR_RSSI_DBM_INVALID);
  REQUIRE(response.status_code == CELLULAR_HTTP_STATUS_INVALID);
}
