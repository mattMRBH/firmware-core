/**
 * AirGradient Go -- LP Core Watchdog Feed
 *
 * Minimal LP Core program: pulse the external hardware watchdog GPIO
 * (LP_IO_2) HIGH for 20 ms, then LOW.  Runs periodically on the LP timer
 * during deep sleep to keep the watchdog alive.
 *
 * The LP Core startup code calls main(), re-arms the LP timer, and halts.
 * On the next LP timer tick the LP Core wakes and runs main() again.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "ulp_lp_core.h"
#include "ulp_lp_core_gpio.h"
#include "ulp_lp_core_utils.h"

#define WDT_LP_IO LP_IO_NUM_2
#define WDT_PULSE_US 20000

int main(void) {
  ulp_lp_core_gpio_init(WDT_LP_IO);
  ulp_lp_core_gpio_output_enable(WDT_LP_IO);
  ulp_lp_core_gpio_set_level(WDT_LP_IO, 1);
  ulp_lp_core_delay_us(WDT_PULSE_US);
  ulp_lp_core_gpio_set_level(WDT_LP_IO, 0);
  return 0;
}
