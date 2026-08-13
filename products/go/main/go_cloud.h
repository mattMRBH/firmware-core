#ifndef GO_CLOUD_H
#define GO_CLOUD_H

#include <cstdint>

class MeasuresAGo;

class CloudService {
public:
  struct Deps {};
  struct Config {
    uint32_t post_interval_ms = 60000;
  };

  CloudService(RtosQueueHandle event_queue, const Deps &deps, const Config &cfg);
  ~CloudService();

  void update_measures_snapshot(const MeasuresAGo &m);

  // Runtime setter for the POST interval (ms). Clamped to a minimum of 60'000
  // ms inside the CloudService implementation to enforce rate limiting.
  void set_post_interval_ms(uint32_t ms);

private:
  Config _cfg;
  void _wake();
};

#endif // GO_CLOUD_H
