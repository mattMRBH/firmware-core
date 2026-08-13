*** Begin Patch
*** Update File: products/go/main/go_ui.cpp
@@
   switch (_editing_setting_id) {
@@
   case SETTING_MEASURE_INTERVAL:
     options = MEASURE_INTERVAL_OPTIONS;
     break;
+  case SETTING_UPDATE_INTERVAL:
+    options = UPDATE_INTERVAL_OPTIONS;
+    break;
*** End Patch
