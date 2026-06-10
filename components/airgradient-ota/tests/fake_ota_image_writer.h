/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_FAKE_OTA_IMAGE_WRITER_H
#define AG_FAKE_OTA_IMAGE_WRITER_H

#include <cstddef>
#include <cstdint>

#include "hal/ota_image_writer.h"

// Host-side fake writer. Records call sequence and byte accounting, and lets a
// test force any primitive to fail via the *_status knobs.
class FakeOtaImageWriter : public OtaImageWriter {
public:
  OtaStatus begin(size_t total_size) override {
    begin_called = true;
    begin_total = total_size;
    _bytes = 0;
    return begin_status;
  }

  OtaStatus write(const uint8_t *data, size_t len) override {
    if (data == nullptr || len == 0) {
      return OtaStatus::InvalidArgument;
    }
    ++write_calls;
    if (write_status != OtaStatus::Ok) {
      return write_status;
    }
    _bytes += len;
    return OtaStatus::Ok;
  }

  OtaStatus finish() override {
    finish_called = true;
    return finish_status;
  }

  void abort() override { ++abort_calls; }

  size_t bytes_written() const override { return _bytes; }

  // Failure knobs.
  OtaStatus begin_status = OtaStatus::Ok;
  OtaStatus write_status = OtaStatus::Ok;
  OtaStatus finish_status = OtaStatus::Ok;

  // Observed state.
  bool begin_called = false;
  bool finish_called = false;
  size_t begin_total = 0;
  int write_calls = 0;
  int abort_calls = 0;

private:
  size_t _bytes = 0;
};

#endif // AG_FAKE_OTA_IMAGE_WRITER_H
