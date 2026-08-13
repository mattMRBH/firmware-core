*** Begin Patch
*** Update File: products/go/main/go_ui.cpp
@@
 uint8_t UIManager::setting_option_count(uint8_t setting_id) const {
   switch (setting_id) {
@@
   case SETTING_MEASURE_INTERVAL:
     return MEASURE_INTERVAL_COUNT;
+  case SETTING_UPDATE_INTERVAL:
+    return UPDATE_INTERVAL_COUNT;
@@
 }
@@
 uint8_t UIManager::setting_current_option(uint8_t setting_id) const {
   switch (setting_id) {
@@
   case SETTING_MEASURE_INTERVAL:
     return _setting_measure_interval;
+  case SETTING_UPDATE_INTERVAL:
+    return _setting_update_interval;
@@
 }
@@
   switch (_editing_setting_id) {
@@
   case SETTING_MEASURE_INTERVAL:
     _setting_measure_interval = option_index;
     break;
+  case SETTING_UPDATE_INTERVAL:
+    _setting_update_interval = option_index;
+    break;
@@
 }
*** End Patch
