/**
 * AirGradient Go — Application Entry Point
 *
 * Thin shell: constructs the real board and runs the app.
 *
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include "go_app.h"
#include "go_hardware_board.h"

extern "C" void app_main() {
  GoHardwareBoard board;
  GoApp app(board);
  app.run();
}
