#ifndef TEST_BLE_H
#define TEST_BLE_H

#include "sdkconfig.h"

#ifdef CONFIG_BT_NIMBLE_ENABLED

// Exercises NimbleBleServer: initialises the BLE stack, registers a test
// GATT service with one readable+notifiable characteristic, configures the
// advertised name and service UUID, starts advertising, and then continues
// serving reads, writes, and notifications.
// Requires CONFIG_BT_ENABLED=y and CONFIG_BT_NIMBLE_ENABLED=y in sdkconfig.
void run_test_ble(const char *device_name);

#endif // CONFIG_BT_NIMBLE_ENABLED

#endif // TEST_BLE_H
