*** Begin Patch
*** Update File: products/go/main/go_cloud.cpp
@@
   void CloudService::set_config_fetch_enabled(bool enabled) {
@@
 } 
+
+void CloudService::set_post_interval_ms(uint32_t ms) {
+  // Enforce a minimum of 60 seconds (60'000 ms) to protect the server
+  // and keep the device behaviour bounded.
+  const uint32_t min_ms = 60'000;
+  if (ms < min_ms) {
+    AG_LOGW(TAG, "set_post_interval_ms: requested %u ms < min %u ms -> clamping",
+            static_cast<unsigned>(ms), static_cast<unsigned>(min_ms));
+    ms = min_ms;
+  }
+  // Store in the config so the run loop and scheduling logic use the
+  // updated interval. _cfg is owned by the object and used by the run
+  // loop; mutate it here and wake the task so it re-evaluates deadlines.
+  _cfg.post_interval_ms = ms;
+  _wake();
+}
+
*** End Patch
