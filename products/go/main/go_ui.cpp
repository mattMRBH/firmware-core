*** Begin Patch
*** Update File: products/go/main/go_ui.cpp
@@
   settings.measure_interval_seconds = index_to_seconds(_setting_measure_interval);
+
+  // Map Update interval option index back to seconds.
+  static constexpr int UPDATE_INTERVAL_SECONDS[] = {60, 300, 900, 3600, 10800};
+  static constexpr uint8_t UPDATE_INTERVAL_COUNT = 5;
+
+  auto update_index_to_seconds = [](uint8_t index) -> int {
+    if (index < UPDATE_INTERVAL_COUNT)
+      return UPDATE_INTERVAL_SECONDS[index];
+    return UPDATE_INTERVAL_SECONDS[0];
+  };
+
+  settings.update_interval_seconds = update_index_to_seconds(_setting_update_interval);
*** End Patch
