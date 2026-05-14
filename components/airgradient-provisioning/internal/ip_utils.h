/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_PROVISIONING_IP_UTILS_H
#define AG_PROVISIONING_IP_UTILS_H

#include <cstdint>
#include <cstdio>

// Parse a dotted-decimal IPv4 string (e.g. "192.168.1.100") into a
// uint32_t in network byte order (octet 0 in the low byte — matches
// lwIP ip4_addr_t and WifiStaticIpConfig). Returns true on success.
inline bool parse_ipv4(const char *s, uint32_t &out_be) {
  if (s == nullptr) {
    return false;
  }
  unsigned a = 0, b = 0, c = 0, d = 0;
  int matched = std::sscanf(s, "%u.%u.%u.%u", &a, &b, &c, &d);
  if (matched != 4) {
    return false;
  }
  if (a > 255 || b > 255 || c > 255 || d > 255) {
    return false;
  }
  out_be = (static_cast<uint32_t>(a)) | (static_cast<uint32_t>(b) << 8) |
           (static_cast<uint32_t>(c) << 16) | (static_cast<uint32_t>(d) << 24);
  return true;
}

#endif // AG_PROVISIONING_IP_UTILS_H
