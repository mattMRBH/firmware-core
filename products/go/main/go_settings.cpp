*** Begin Patch
*** Update File: products/go/main/go_settings.cpp
@@
 constexpr const char *KEY_MEASURE_INTERVAL_SECONDS = "mi";
+// Cloud POST cadence (seconds)
+constexpr const char *KEY_UPDATE_INTERVAL_SECONDS = "ui";
@@
   int measure_interval_seconds = 0;
   if (store.get_int(KEY_MEASURE_INTERVAL_SECONDS, measure_interval_seconds) ==
           ConfigStoreResult::OK &&
       is_measure_interval_seconds_valid(measure_interval_seconds)) {
     settings.measure_interval_seconds = measure_interval_seconds;
   }
+
+  int update_interval_seconds = 0;
+  if (store.get_int(KEY_UPDATE_INTERVAL_SECONDS, update_interval_seconds) ==
+      ConfigStoreResult::OK) {
+    // Validate against allowed discrete choices: 60, 300, 900, 3600, 10800
+    switch (update_interval_seconds) {
+    case 60:
+    case 300:
+    case 900:
+    case 3600:
+    case 10800:
+      settings.update_interval_seconds = update_interval_seconds;
+      break;
+    default:
+      // Ignore invalid persisted value and keep default
+      break;
+    }
+  }
@@
   if (store.set_int(KEY_MEASURE_INTERVAL_SECONDS, settings.measure_interval_seconds) !=
       ConfigStoreResult::OK) {
     return false;
   }
+
+  if (store.set_int(KEY_UPDATE_INTERVAL_SECONDS, settings.update_interval_seconds) !=
+      ConfigStoreResult::OK) {
+    return false;
+  }
*** End Patch
