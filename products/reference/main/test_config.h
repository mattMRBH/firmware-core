#ifndef TEST_CONFIG_H
#define TEST_CONFIG_H

#include "config_store.h"

// Exercises NvsConfigStore + ReferenceSettings: load, mutate, save, reload,
// verify round-trip, then restore original values.
void run_test_config(ConfigStore &store);

#endif // TEST_CONFIG_H
