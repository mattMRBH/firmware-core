*** Begin Patch
*** Update File: products/go/main/go_settings.h
@@
-  // --- Measurement interval ---
-  int measure_interval_seconds = MEASURE_INTERVAL_SECONDS_DEFAULT;
+  // --- Measurement interval ---
+  int measure_interval_seconds = MEASURE_INTERVAL_SECONDS_DEFAULT;
+
+  // --- Cloud POST cadence (seconds) ---
+  // Persisted user choice for how frequently the device posts measurements
+  // to the cloud when Stationary. Allowed values: 60, 300, 900, 3600, 10800.
+  int update_interval_seconds = 60;
*** End Patch
