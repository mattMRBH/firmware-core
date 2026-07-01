/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_MOCK_OTA_IMAGE_SOURCE_H
#define AG_MOCK_OTA_IMAGE_SOURCE_H

#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include "hal/ota_image_source.h"

// Trompeloeil mock of the pull transport seam. Used by the OtaUpdater tests to
// verify open -> read -> close ordering and error handling.
class MockOtaImageSource : public trompeloeil::mock_interface<OtaImageSource> {
public:
  IMPLEMENT_MOCK1(open);
  IMPLEMENT_MOCK2(read);
  IMPLEMENT_MOCK0(close);
};

#endif // AG_MOCK_OTA_IMAGE_SOURCE_H
