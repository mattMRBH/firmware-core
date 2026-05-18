#ifndef TEST_AIRGRADIENT_CLIENT_H
#define TEST_AIRGRADIENT_CLIENT_H

// AirGradient client smoke test.
//
// Brings up WiFi STA (credentials are file-local constants in the .cpp),
// waits for a DHCP-assigned IP, then runs a small table of cases against
// the live AG backend to verify AgClient's response-code mapping:
//
//   - happy path:  registered serial number on configured domain
//                  -> fetch=Ok,           post=Ok
//   - unregistered serial number on configured domain
//                  -> fetch=NotRegistered, post=ServerError
//   - wrong endpoint (DNS-unresolvable host)
//                  -> fetch=TransportError, post=TransportError
//
// Each case logs got/expect for fetch and post; a final summary tallies
// pass/fail.  Runs once and returns; WiFi stays up afterwards.
void run_test_airgradient_client();

#endif // TEST_AIRGRADIENT_CLIENT_H
