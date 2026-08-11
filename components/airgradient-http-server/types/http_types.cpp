/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "types/http_types.h"

const char *http_status_phrase(HttpStatus status) {
  switch (status) {
  case HttpStatus::Ok:
    return "200 OK";
  case HttpStatus::Created:
    return "201 Created";
  case HttpStatus::Accepted:
    return "202 Accepted";
  case HttpStatus::NoContent:
    return "204 No Content";
  case HttpStatus::Found:
    return "302 Found";
  case HttpStatus::BadRequest:
    return "400 Bad Request";
  case HttpStatus::Forbidden:
    return "403 Forbidden";
  case HttpStatus::NotFound:
    return "404 Not Found";
  case HttpStatus::MethodNotAllowed:
    return "405 Method Not Allowed";
  case HttpStatus::InternalServerError:
    return "500 Internal Server Error";
  case HttpStatus::ServiceUnavailable:
    return "503 Service Unavailable";
  }
  return "500 Internal Server Error";
}
