/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef DRIVERS_HTTP_BODY_READER_H
#define DRIVERS_HTTP_BODY_READER_H

#include <cstddef>
#include <string>

namespace http_internal {

enum class BodyReadStatus {
  Complete,
  TooLarge,
  ShortRead,
  ReceiveError,
};

template <typename Receive>
BodyReadStatus read_body(std::string &body, size_t content_length, size_t capacity,
                         Receive receive) {
  body.clear();
  if (content_length == 0) {
    return BodyReadStatus::Complete;
  }
  if (content_length > capacity) {
    return BodyReadStatus::TooLarge;
  }

  body.resize(content_length);
  size_t offset = 0;
  while (offset < content_length) {
    const size_t remaining = content_length - offset;
    const int received = receive(body.data() + offset, remaining);
    if (received <= 0) {
      body.clear();
      return received == 0 ? BodyReadStatus::ShortRead : BodyReadStatus::ReceiveError;
    }
    if (static_cast<size_t>(received) > remaining) {
      body.clear();
      return BodyReadStatus::ReceiveError;
    }
    offset += static_cast<size_t>(received);
  }
  return BodyReadStatus::Complete;
}

} // namespace http_internal

#endif // DRIVERS_HTTP_BODY_READER_H
