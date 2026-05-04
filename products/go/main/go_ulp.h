/**
 * AirGradient Go -- LP Core Watchdog Interface
 *
 * Start/stop the LP Core watchdog feed program that pulses GPIO2 during
 * deep sleep, keeping the external hardware watchdog alive.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef GO_ULP_H
#define GO_ULP_H

/// Start the LP Core watchdog feed program.
///
/// Loads the LP Core binary, switches GPIO2 to RTCIO mode, and starts
/// the LP Core with a 60 s periodic timer.  Call after the final
/// main-CPU watchdog pulse, just before enter_sleep().
void ulp_wdt_start();

/// Stop the LP Core watchdog feed program.
///
/// Waits briefly for any in-progress watchdog pulse to complete, then
/// halts the LP Core and clears its wakeup sources.  Safe to call when
/// the LP Core is not running (no-op).
///
/// Does NOT switch GPIO2 back to regular GPIO mode -- that happens
/// naturally when init_ext_watchdog() is called later in the boot path.
void ulp_wdt_stop();

#endif // GO_ULP_H
