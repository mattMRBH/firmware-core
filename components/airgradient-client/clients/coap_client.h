/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_COAP_CLIENT_H
#define AG_COAP_CLIENT_H

#include <cstddef>
#include <cstdint>

// CoapClient encapsulates all CoAP protocol machinery -- packet build/parse,
// CON/ACK handling, Block1 chunking, retry, DNS fallback.  AgClient never
// sees CoAP packet internals.  Interface defined now for completeness;
// concrete backends are future work.
class CoapClient {
public:
  virtual ~CoapClient() = default;

  // Fetch config from a CoAP server.  uri_path is the CoAP URI path (e.g.
  // serial number).  See HttpClient::get() for the buffer contract; CoAP
  // does not support truncation reporting separately -- callers must size
  // the buffer to the maximum expected response.
  virtual bool get(const char *host, int port, const char *uri_path, char *response_body,
                   size_t body_size, size_t *bytes_written) = 0;

  // Post binary payload.  Handles Block1 chunking internally when the
  // payload exceeds the block size.  response_code_class/detail receive the
  // CoAP response code split (e.g. 2.04 -> class=2, detail=4).
  virtual bool post(const char *host, int port, const char *uri_path, const uint8_t *body,
                    size_t body_len, int &response_code_class, int &response_code_detail) = 0;
};

#endif // AG_COAP_CLIENT_H
