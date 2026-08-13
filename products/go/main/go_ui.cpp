*** Begin Patch
*** Update File: products/go/main/go_ui.cpp
@@
   _setting_units = s.use_fahrenheit ? 1 : 0;
@@
   _setting_measure_interval = seconds_to_index(s.measure_interval_seconds, false);
+
+  // Update interval (POST cadence): discrete choices {60,300,900,3600,10800}
+  switch (s.update_interval_seconds) {
+  case 60:
+    _setting_update_interval = 0;
+    break;
+  case 300:
+    _setting_update_interval = 1;
+    break;
+  case 900:
+    _setting_update_interval = 2;
+    break;
+  case 3600:
+    _setting_update_interval = 3;
+    break;
+  case 10800:
+    _setting_update_interval = 4;
+    break;
+  default:
+    _setting_update_interval = 0; // default 1m
+    break;
+  }
*** End Patch
