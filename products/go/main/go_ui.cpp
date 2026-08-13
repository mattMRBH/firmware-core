*** Begin Patch
*** Update File: products/go/main/go_ui.cpp
@@
   switch (item_index) {
@@
     case SETTING_MEASURE_INTERVAL:
-      (void)snprintf(label, sizeof(label), "Measure Int.: %s",
-                     MEASURE_INTERVAL_OPTIONS[_setting_measure_interval]);
+      (void)snprintf(label, sizeof(label), "Measure Int.: %s",
+                     MEASURE_INTERVAL_OPTIONS[_setting_measure_interval]);
+      break;
+    case SETTING_UPDATE_INTERVAL:
+      (void)snprintf(label, sizeof(label), "Update Int.: %s",
+                     UPDATE_INTERVAL_OPTIONS[_setting_update_interval]);
       break;
*** End Patch
