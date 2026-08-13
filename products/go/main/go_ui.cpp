*** Begin Patch
*** Update File: products/go/main/go_ui.cpp
@@
 static const char *const MEASURE_INTERVAL_OPTIONS[] = {"3s", "10s", "30s", "60s",
-                                                       "5m", "15m", "1h"};
+                                                       "5m", "15m", "1h"};
 static constexpr uint8_t MEASURE_INTERVAL_COUNT = 7;
+
+static const char *const UPDATE_INTERVAL_OPTIONS[] = {"1m", "5m", "15m", "1h", "3h"};
+static constexpr uint8_t UPDATE_INTERVAL_COUNT = 5;
@@
 static constexpr uint8_t SETTING_MEASURE_INTERVAL = 5;
+static constexpr uint8_t SETTING_UPDATE_INTERVAL = 6;
 static constexpr uint8_t SETTING_GPS_MODE = 6;
@@
   static constexpr uint8_t SETTINGS_TOTAL = 17;       // indices 0..16
*** End Patch
