*** Begin Patch
*** Update File: products/go/main/go_cloud.h
@@
   CloudService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg);
   ~CloudService();
@@
   void update_measures_snapshot(const MeasuresAGo &m);
+
+  // Runtime setter for the POST interval (ms). Clamped to a minimum of 60'000
+  // ms inside the CloudService implementation to enforce rate limiting.
+  void set_post_interval_ms(uint32_t ms);
*** End Patch
