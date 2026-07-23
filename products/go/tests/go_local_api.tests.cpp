/**
 * AirGradient Go -- local API product service host tests
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/catch_approx.hpp>

#include <cmath>
#include <cstring>
#include <limits>
#include <memory>
#include <optional>
#include <string>

#include "go_events.h"
#include "go_local_api.h"
#include "rtos.h"

class GoLocalApiServiceTestAccess {
public:
  static size_t request_count(const GoLocalApiService &service) { return service._count; }

  static ConfigSubmitResult admit_config(GoLocalApiService &service, const GoConfigUpdate &update,
                                         uint32_t expected_epoch) {
    return service.admit_config(update, false, expected_epoch);
  }
};

namespace {

constexpr const char *TEST_SERIAL = "aabbccddeeff";
constexpr const char *TEST_FIRMWARE = "1.2.3";

class TestRtos final : public RTOS {
public:
  void delay_ms_impl(uint32_t) override {}
  uint64_t get_time_ms_impl() override { return 0; }

  bool queue_send_impl(RtosQueueHandle queue_handle, const void *item,
                       uint32_t timeout_ms) override {
    last_send_timeout_ms = timeout_ms;
    if (reject_queue_send) {
      return false;
    }
    return RTOS::queue_send_impl(queue_handle, item, timeout_ms);
  }

  bool reject_queue_send = false;
  uint32_t last_send_timeout_ms = UINT32_MAX;
};

struct Fixture {
  Fixture() {
    RTOS::set_instance(&rtos);
    event_queue = RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));
    REQUIRE(event_queue != nullptr);

    GoLocalApiService::Config config{};
    config.serial_number = TEST_SERIAL;
    config.firmware_version = TEST_FIRMWARE;
    service = std::make_unique<GoLocalApiService>(event_queue, config);
  }

  ~Fixture() {
    service.reset();
    RTOS::queue_delete(event_queue);
    RTOS::set_instance(nullptr);
  }

  Event receive_event() {
    Event event{};
    REQUIRE(RTOS::queue_receive(event_queue, &event, 0));
    return event;
  }

  LocalApiRequest receive_request() {
    const Event event = receive_event();
    REQUIRE(event.type == EventType::LocalApiRequestReady);
    LocalApiRequest request{};
    REQUIRE(service->pop_request(event.local_api_epoch, request));
    return request;
  }

  TestRtos rtos;
  RtosQueueHandle event_queue = nullptr;
  std::unique_ptr<GoLocalApiService> service;
};

uint32_t field_mask(GoConfigField field) { return static_cast<uint32_t>(field); }

LocalServerConfig pm_standard_config(const char *value) {
  LocalServerConfig config{};
  config.pm_standard = value;
  return config;
}

CorrectionEntry custom_pm25(double intercept, double scaling_factor, bool use_epa2021) {
  CorrectionEntry entry{};
  entry.algorithm = "custom_via_pm25_raw";
  SlrParams slr{};
  slr.intercept = intercept;
  slr.scaling_factor = scaling_factor;
  slr.use_epa2021 = use_epa2021;
  entry.slr = slr;
  return entry;
}

CorrectionEntry custom_linear(double intercept, double scaling_factor) {
  CorrectionEntry entry{};
  entry.algorithm = "custom";
  SlrParams slr{};
  slr.intercept = intercept;
  slr.scaling_factor = scaling_factor;
  entry.slr = slr;
  return entry;
}

LocalServerConfig correction_config(const std::optional<CorrectionEntry> &pm25,
                                    const std::optional<CorrectionEntry> &temperature,
                                    const std::optional<CorrectionEntry> &humidity) {
  LocalServerConfig config{};
  Corrections corrections{};
  corrections.pm25 = pm25;
  corrections.temp = temperature;
  corrections.humidity = humidity;
  config.corrections = corrections;
  return config;
}

void require_status(const ConfigSubmitResult &result, ConfigSubmitStatus status,
                    ConfigFieldId field = ConfigFieldId::None) {
  REQUIRE(result.status == status);
  REQUIRE(result.field == field);
}

} // namespace

TEST_CASE("Go local API initializes safe snapshots") {
  Fixture fixture;

  const Measures measures = fixture.service->get_measures();
  CHECK_FALSE(measures.co2.is_valid());
  CHECK_FALSE(measures.pm_a.is_pm_01_valid());
  CHECK_FALSE(measures.pm_a.is_pm_25_valid());
  CHECK_FALSE(measures.pm_a.is_pm_10_valid());
  CHECK_FALSE(measures.pm_a.is_pm_03_pc_valid());
  CHECK_FALSE(measures.temp_hum_a.is_temp_valid());
  CHECK_FALSE(measures.temp_hum_a.is_hum_valid());
  CHECK_FALSE(measures.tvoc_nox.is_tvoc_index_valid());
  CHECK_FALSE(measures.tvoc_nox.is_tvoc_raw_valid());
  CHECK_FALSE(measures.tvoc_nox.is_nox_index_valid());
  CHECK_FALSE(measures.tvoc_nox.is_nox_raw_valid());

  const SystemInfo info = fixture.service->get_system_info();
  CHECK(std::string(info.serial_number) == TEST_SERIAL);
  CHECK(std::string(info.model) == "P-1PSG");
  CHECK(std::string(info.firmware) == TEST_FIRMWARE);
  CHECK_FALSE(info.wifi_rssi.has_value());
  CHECK(info.boot == 0);

  const LocalServerConfig config = fixture.service->get_config();
  REQUIRE(config.pm_standard.has_value());
  REQUIRE(config.temperature_unit.has_value());
  REQUIRE(config.cloud_connection.has_value());
  REQUIRE(config.configuration_control.has_value());
  CHECK(*config.pm_standard == "ugm3");
  CHECK(*config.temperature_unit == "c");
  CHECK(*config.cloud_connection);
  CHECK(*config.configuration_control == "both");
  CHECK(fixture.service->access() == ConfigAccess::Disabled);
  CHECK(fixture.service->queue_epoch() == 0);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 0);
  CHECK(fixture.service->is_valid());
}

TEST_CASE("Go local API truncates identity while preserving termination") {
  TestRtos rtos;
  RTOS::set_instance(&rtos);
  RtosQueueHandle queue = RTOS::queue_create(EVENT_QUEUE_DEPTH, sizeof(Event));
  REQUIRE(queue != nullptr);

  const std::string serial(64, 's');
  const std::string firmware(64, 'f');
  GoLocalApiService::Config config{};
  config.serial_number = serial.c_str();
  config.firmware_version = firmware.c_str();
  GoLocalApiService service(queue, config);

  const SystemInfo info = service.get_system_info();
  CHECK(info.serial_number[sizeof(info.serial_number) - 1] == '\0');
  CHECK(info.firmware[sizeof(info.firmware) - 1] == '\0');
  CHECK(std::strlen(info.serial_number) == sizeof(info.serial_number) - 1);
  CHECK(std::strlen(info.firmware) == sizeof(info.firmware) - 1);

  RTOS::queue_delete(queue);
  RTOS::set_instance(nullptr);
}

TEST_CASE("Go local API publishes corrected supported measures field by field") {
  Fixture fixture;
  MeasuresAGo corrected{};
  corrected.co2.co2 = 612;
  corrected.pm_a.pm_01 = 1.1f;
  corrected.pm_a.pm_25 = 2.5f;
  corrected.pm_a.pm_10 = 10.2f;
  corrected.pm_a.pm_03_pc = 321.0f;
  corrected.temp_hum_a.temperature = 24.5f;
  corrected.temp_hum_a.humidity = 47.0f;
  corrected.tvoc_nox.tvoc_index = 100;
  corrected.tvoc_nox.tvoc_raw = 200;
  corrected.tvoc_nox.nox_index = 3;
  corrected.tvoc_nox.nox_raw = 4;
  corrected.power.battery_voltage = 4.1f;
  corrected.pressure.pressure = 1013.0f;

  fixture.service->publish_measurement_snapshot(corrected, 7);
  const Measures measures = fixture.service->get_measures();
  CHECK(measures.co2.co2 == 612);
  CHECK(measures.pm_a.pm_01 == 1.1f);
  CHECK(measures.pm_a.pm_25 == 2.5f);
  CHECK(measures.pm_a.pm_10 == 10.2f);
  CHECK(measures.pm_a.pm_03_pc == 321.0f);
  CHECK(measures.temp_hum_a.temperature == 24.5f);
  CHECK(measures.temp_hum_a.humidity == 47.0f);
  CHECK(measures.tvoc_nox.tvoc_index == 100);
  CHECK(measures.tvoc_nox.tvoc_raw == 200);
  CHECK(measures.tvoc_nox.nox_index == 3);
  CHECK(measures.tvoc_nox.nox_raw == 4);
  CHECK_FALSE(measures.power.is_valid());
  CHECK_FALSE(measures.pressure.is_valid());
  CHECK_FALSE(measures.temp_hum_b.is_valid());
  CHECK_FALSE(measures.pm_b.is_valid());
  CHECK_FALSE(measures.electrode.is_valid());
  CHECK(fixture.service->get_system_info().boot == 7);

  corrected.pm_a.pm_25 = std::numeric_limits<float>::infinity();
  corrected.temp_hum_a.temperature = std::numeric_limits<float>::quiet_NaN();
  corrected.tvoc_nox.nox_raw = MeasuresInvalid::NOX;
  fixture.service->publish_measurement_snapshot(corrected, 8);
  const Measures replaced = fixture.service->get_measures();
  CHECK_FALSE(replaced.pm_a.is_pm_25_valid());
  CHECK_FALSE(replaced.temp_hum_a.is_temp_valid());
  CHECK_FALSE(replaced.tvoc_nox.is_nox_raw_valid());
  CHECK(replaced.pm_a.pm_01 == 1.1f);
  CHECK(fixture.service->get_system_info().boot == 8);

  fixture.service->publish_measurement_snapshot(MeasuresAGo{}, 9);
  const Measures invalid = fixture.service->get_measures();
  CHECK_FALSE(invalid.co2.is_valid());
  CHECK_FALSE(invalid.pm_a.is_pm_01_valid());
  CHECK_FALSE(invalid.pm_a.is_pm_25_valid());
  CHECK_FALSE(invalid.pm_a.is_pm_10_valid());
  CHECK_FALSE(invalid.pm_a.is_pm_03_pc_valid());
  CHECK_FALSE(invalid.temp_hum_a.is_temp_valid());
  CHECK_FALSE(invalid.temp_hum_a.is_hum_valid());
  CHECK_FALSE(invalid.tvoc_nox.is_tvoc_index_valid());
  CHECK_FALSE(invalid.tvoc_nox.is_tvoc_raw_valid());
  CHECK_FALSE(invalid.tvoc_nox.is_nox_index_valid());
  CHECK_FALSE(invalid.tvoc_nox.is_nox_raw_valid());
}

TEST_CASE("Go local API publishes optional RSSI independently") {
  Fixture fixture;
  fixture.service->publish_wifi_rssi(-61);
  REQUIRE(fixture.service->get_system_info().wifi_rssi.has_value());
  CHECK(*fixture.service->get_system_info().wifi_rssi == -61);

  fixture.service->publish_wifi_rssi(std::nullopt);
  CHECK_FALSE(fixture.service->get_system_info().wifi_rssi.has_value());
}

TEST_CASE("Go local API maps the supported active config subset") {
  Fixture fixture;
  GoSettings settings{};
  settings.pm_use_usaqi = true;
  settings.use_fahrenheit = true;
  settings.disable_cloud = true;
  settings.configuration_control = ConfigurationControl::Local;
  settings.corrections.pm25 = {Pm25CorrectionAlgorithm::CustomViaPm25Raw, 0.5f, -1.5f, true};
  settings.corrections.temperature = {LinearCorrectionAlgorithm::Custom, 1.2f, -2.0f};
  settings.corrections.humidity = {LinearCorrectionAlgorithm::Custom, 0.8f, 3.0f};
  fixture.service->publish_config_snapshot(settings);

  const LocalServerConfig config = fixture.service->get_config();
  CHECK(*config.pm_standard == "us-aqi");
  CHECK(*config.temperature_unit == "f");
  CHECK_FALSE(*config.cloud_connection);
  CHECK(*config.configuration_control == "local");
  CHECK_FALSE(config.country.has_value());
  CHECK_FALSE(config.post_data_to_cloud.has_value());
  CHECK_FALSE(config.co2_abc_days.has_value());
  CHECK_FALSE(config.tvoc_learning_offset.has_value());
  CHECK_FALSE(config.nox_learning_offset.has_value());
  CHECK_FALSE(config.led_mode.has_value());
  CHECK_FALSE(config.led_bar_brightness.has_value());
  CHECK_FALSE(config.display_brightness.has_value());
  CHECK_FALSE(config.mqtt_broker_url.has_value());
  CHECK_FALSE(config.http_domain.has_value());

  REQUIRE(config.corrections.has_value());
  REQUIRE(config.corrections->pm25.has_value());
  REQUIRE(config.corrections->pm25->slr.has_value());
  CHECK(config.corrections->pm25->algorithm == "custom_via_pm25_raw");
  CHECK(*config.corrections->pm25->slr->intercept == -1.5);
  CHECK(*config.corrections->pm25->slr->scaling_factor == 0.5);
  CHECK(*config.corrections->pm25->slr->use_epa2021);
  REQUIRE(config.corrections->temp.has_value());
  REQUIRE(config.corrections->temp->slr.has_value());
  CHECK(config.corrections->temp->algorithm == "custom");
  CHECK(*config.corrections->temp->slr->intercept == -2.0);
  CHECK(*config.corrections->temp->slr->scaling_factor == Catch::Approx(1.2));
  CHECK_FALSE(config.corrections->temp->slr->use_epa2021.has_value());
  REQUIRE(config.corrections->humidity.has_value());
  REQUIRE(config.corrections->humidity->slr.has_value());
  CHECK(config.corrections->humidity->algorithm == "custom");
}

TEST_CASE("Go local API emits canonical disabled and EPA correction shapes") {
  Fixture fixture;
  GoSettings settings{};
  settings.configuration_control = ConfigurationControl::Cloud;
  settings.corrections.pm25.algorithm = Pm25CorrectionAlgorithm::Epa2021;
  fixture.service->publish_config_snapshot(settings);

  const LocalServerConfig config = fixture.service->get_config();
  CHECK(*config.configuration_control == "cloud");
  REQUIRE(config.corrections.has_value());
  CHECK(config.corrections->pm25->algorithm == "epa_2021");
  CHECK_FALSE(config.corrections->pm25->slr.has_value());
  CHECK(config.corrections->temp->algorithm == "none");
  CHECK_FALSE(config.corrections->temp->slr.has_value());
  CHECK(config.corrections->humidity->algorithm == "none");
  CHECK_FALSE(config.corrections->humidity->slr.has_value());
}

TEST_CASE("Go local API translates one atomic supported update") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);

  LocalServerConfig partial{};
  partial.pm_standard = "us-aqi";
  partial.temperature_unit = "f";
  partial.cloud_connection = false;
  partial.configuration_control = "local";
  Corrections corrections{};
  corrections.pm25 = custom_pm25(-1.0, 0.25, true);
  corrections.temp = custom_linear(2.0, 1.1);
  corrections.humidity = custom_linear(-3.0, 0.9);
  partial.corrections = corrections;

  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::Accepted);
  CHECK(fixture.rtos.last_send_timeout_ms == 0);
  const LocalApiRequest request = fixture.receive_request();
  REQUIRE(request.kind == LocalApiRequestKind::Config);
  const uint32_t expected_mask =
      field_mask(GoConfigField::PmStandard) | field_mask(GoConfigField::TemperatureUnit) |
      field_mask(GoConfigField::CloudConnection) | field_mask(GoConfigField::ConfigurationControl) |
      field_mask(GoConfigField::Pm25Correction) | field_mask(GoConfigField::TemperatureCorrection) |
      field_mask(GoConfigField::HumidityCorrection);
  CHECK(request.config.update_mask == expected_mask);
  CHECK(request.config.pm_use_usaqi);
  CHECK(request.config.use_fahrenheit);
  CHECK(request.config.disable_cloud);
  CHECK(request.config.configuration_control == ConfigurationControl::Local);
  CHECK(request.config.corrections.pm25.algorithm == Pm25CorrectionAlgorithm::CustomViaPm25Raw);
  CHECK(request.config.corrections.pm25.intercept == -1.0f);
  CHECK(request.config.corrections.pm25.scaling_factor == 0.25f);
  CHECK(request.config.corrections.pm25.use_epa2021);
  CHECK(request.config.corrections.temperature.algorithm == LinearCorrectionAlgorithm::Custom);
  CHECK(request.config.corrections.temperature.intercept == 2.0f);
  CHECK(request.config.corrections.temperature.scaling_factor == Catch::Approx(1.1f));
  CHECK(request.config.corrections.humidity.algorithm == LinearCorrectionAlgorithm::Custom);
  CHECK(request.config.corrections.humidity.intercept == -3.0f);
  CHECK(request.config.corrections.humidity.scaling_factor == Catch::Approx(0.9f));
}

TEST_CASE("Go local API preserves absent active correction siblings") {
  Fixture fixture;
  GoSettings settings{};
  settings.corrections.temperature = {LinearCorrectionAlgorithm::Custom, 2.0f, 3.0f};
  settings.corrections.humidity = {LinearCorrectionAlgorithm::Custom, 4.0f, 5.0f};
  fixture.service->publish_config_snapshot(settings);
  fixture.service->set_access(ConfigAccess::ReadWrite);

  const LocalServerConfig partial =
      correction_config(custom_pm25(1.0, 0.5, false), std::nullopt, std::nullopt);
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::Accepted);
  const LocalApiRequest request = fixture.receive_request();
  CHECK(request.config.update_mask == field_mask(GoConfigField::Pm25Correction));
  CHECK(request.config.corrections.temperature.scaling_factor == 2.0f);
  CHECK(request.config.corrections.temperature.intercept == 3.0f);
  CHECK(request.config.corrections.humidity.scaling_factor == 4.0f);
  CHECK(request.config.corrections.humidity.intercept == 5.0f);
}

TEST_CASE("Go local API enforces access and source policy before field support") {
  Fixture fixture;
  LocalServerConfig unsupported{};
  unsupported.country = "US";

  require_status(fixture.service->submit_config(unsupported), ConfigSubmitStatus::Forbidden);
  fixture.service->set_access(ConfigAccess::ReadOnly);
  require_status(fixture.service->submit_config(unsupported), ConfigSubmitStatus::Forbidden);

  fixture.service->set_access(ConfigAccess::ReadWrite);
  GoSettings cloud_control{};
  cloud_control.configuration_control = ConfigurationControl::Cloud;
  fixture.service->publish_config_snapshot(cloud_control);
  require_status(fixture.service->submit_config(unsupported), ConfigSubmitStatus::Forbidden);

  GoSettings both_control{};
  fixture.service->publish_config_snapshot(both_control);
  require_status(fixture.service->submit_config(unsupported), ConfigSubmitStatus::NotSupported,
                 ConfigFieldId::CountryCode);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 0);

  GoSettings local_control{};
  local_control.configuration_control = ConfigurationControl::Local;
  fixture.service->publish_config_snapshot(local_control);
  require_status(fixture.service->submit_config(pm_standard_config("us-aqi")),
                 ConfigSubmitStatus::Accepted);
  CHECK(fixture.receive_request().config.pm_use_usaqi);
}

TEST_CASE("Go local API permits only exact control recovery from cloud control") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  GoSettings settings{};
  settings.configuration_control = ConfigurationControl::Cloud;
  fixture.service->publish_config_snapshot(settings);

  LocalServerConfig recovery{};
  recovery.configuration_control = "local";
  require_status(fixture.service->submit_config(recovery), ConfigSubmitStatus::Accepted);
  const LocalApiRequest request = fixture.receive_request();
  CHECK(request.config.update_mask == field_mask(GoConfigField::ConfigurationControl));
  CHECK(request.config.configuration_control == ConfigurationControl::Local);

  recovery.configuration_control = "both";
  require_status(fixture.service->submit_config(recovery), ConfigSubmitStatus::Accepted);
  fixture.receive_request();

  recovery.configuration_control = "cloud";
  require_status(fixture.service->submit_config(recovery), ConfigSubmitStatus::Forbidden);
  recovery.configuration_control = "invalid";
  require_status(fixture.service->submit_config(recovery), ConfigSubmitStatus::Forbidden);

  recovery.configuration_control = "local";
  recovery.pm_standard = "ugm3";
  require_status(fixture.service->submit_config(recovery), ConfigSubmitStatus::Forbidden);
  recovery.pm_standard.reset();
  recovery.corrections = Corrections{};
  require_status(fixture.service->submit_config(recovery), ConfigSubmitStatus::Forbidden);

  LocalServerConfig empty{};
  require_status(fixture.service->submit_config(empty), ConfigSubmitStatus::Forbidden);
}

TEST_CASE("Go local API accepts empty updates without queue admission") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  LocalServerConfig empty{};
  require_status(fixture.service->submit_config(empty), ConfigSubmitStatus::Accepted);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 0);

  LocalServerConfig empty_corrections{};
  empty_corrections.corrections = Corrections{};
  require_status(fixture.service->submit_config(empty_corrections), ConfigSubmitStatus::Accepted);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 0);
  Event event{};
  CHECK_FALSE(RTOS::queue_receive(fixture.event_queue, &event, 0));

  for (size_t i = 0; i < LOCAL_API_REQUEST_QUEUE_DEPTH; ++i) {
    const char *value = (i % 2 == 0) ? "ugm3" : "us-aqi";
    require_status(fixture.service->submit_config(pm_standard_config(value)),
                   ConfigSubmitStatus::Accepted);
  }
  require_status(fixture.service->submit_config(empty), ConfigSubmitStatus::Accepted);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) ==
        LOCAL_API_REQUEST_QUEUE_DEPTH);
}

TEST_CASE("Go local API reports deterministic unsupported fields") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);

  LocalServerConfig partial{};
  partial.post_data_to_cloud = true;
  partial.co2_abc_days = 7;
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::NotSupported,
                 ConfigFieldId::PostDataToCloud);

  partial = LocalServerConfig{};
  partial.co2_abc_days = 7;
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::NotSupported,
                 ConfigFieldId::Co2AbcDays);
  partial = LocalServerConfig{};
  partial.tvoc_learning_offset = 1;
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::NotSupported,
                 ConfigFieldId::TvocLearningOffset);
  partial = LocalServerConfig{};
  partial.nox_learning_offset = 1;
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::NotSupported,
                 ConfigFieldId::NoxLearningOffset);
  partial = LocalServerConfig{};
  partial.led_mode = "co2";
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::NotSupported,
                 ConfigFieldId::LedMode);
  partial = LocalServerConfig{};
  partial.led_bar_brightness = 50;
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::NotSupported,
                 ConfigFieldId::LedBarBrightness);
  partial = LocalServerConfig{};
  partial.display_brightness = 50;
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::NotSupported,
                 ConfigFieldId::DisplayBrightness);
  partial = LocalServerConfig{};
  partial.mqtt_broker_url = "mqtt://example";
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::NotSupported,
                 ConfigFieldId::MqttBrokerUrl);
  partial = LocalServerConfig{};
  partial.http_domain = "example.com";
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::NotSupported,
                 ConfigFieldId::HttpDomain);
}

TEST_CASE("Go local API rejects invalid scalar values and cross-field candidates") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);

  LocalServerConfig partial{};
  partial.pm_standard = "US-AQI";
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                 ConfigFieldId::PmStandard);
  partial = LocalServerConfig{};
  partial.temperature_unit = "fahrenheit";
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                 ConfigFieldId::TemperatureUnit);
  partial = LocalServerConfig{};
  partial.configuration_control = "remote";
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                 ConfigFieldId::ConfigurationControl);

  GoSettings local_disabled{};
  local_disabled.disable_cloud = true;
  local_disabled.configuration_control = ConfigurationControl::Local;
  fixture.service->publish_config_snapshot(local_disabled);
  partial = LocalServerConfig{};
  partial.configuration_control = "cloud";
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                 ConfigFieldId::ConfigurationControl);

  GoSettings cloud_enabled{};
  cloud_enabled.configuration_control = ConfigurationControl::Cloud;
  fixture.service->publish_config_snapshot(cloud_enabled);
  partial = LocalServerConfig{};
  partial.cloud_connection = false;
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::Forbidden);

  GoSettings both_enabled{};
  fixture.service->publish_config_snapshot(both_enabled);
  partial.configuration_control = "cloud";
  require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                 ConfigFieldId::ConfigurationControl);
}

TEST_CASE("Go local API validates strict correction shapes") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);

  SECTION("PM custom requires all coefficients and EPA flag") {
    CorrectionEntry entry{};
    entry.algorithm = "custom_via_pm25_raw";
    entry.slr = SlrParams{};
    LocalServerConfig partial = correction_config(entry, std::nullopt, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsPm25);

    entry.slr->scaling_factor = 1.0;
    entry.slr->use_epa2021 = false;
    partial = correction_config(entry, std::nullopt, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsPm25);

    entry.slr = SlrParams{};
    entry.slr->intercept = 0.0;
    entry.slr->use_epa2021 = false;
    partial = correction_config(entry, std::nullopt, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsPm25);

    entry.slr->scaling_factor = 1.0;
    entry.slr->use_epa2021.reset();
    partial = correction_config(entry, std::nullopt, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsPm25);
  }

  SECTION("linear custom requires each coefficient") {
    CorrectionEntry entry{};
    entry.algorithm = "custom";
    entry.slr = SlrParams{};
    entry.slr->scaling_factor = 1.0;
    LocalServerConfig partial = correction_config(std::nullopt, entry, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsTemp);

    entry.slr = SlrParams{};
    entry.slr->intercept = 0.0;
    partial = correction_config(std::nullopt, entry, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsTemp);

    entry.slr = SlrParams{};
    entry.slr->scaling_factor = 1.0;
    partial = correction_config(std::nullopt, std::nullopt, entry);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsHumidity);

    entry.slr = SlrParams{};
    entry.slr->intercept = 0.0;
    partial = correction_config(std::nullopt, std::nullopt, entry);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsHumidity);
  }

  SECTION("non-custom algorithms reject SLR") {
    CorrectionEntry entry{};
    entry.algorithm = "none";
    entry.slr = SlrParams{};
    LocalServerConfig partial = correction_config(entry, std::nullopt, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsPm25);

    entry.algorithm = "none";
    partial = correction_config(std::nullopt, entry, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsTemp);
    partial = correction_config(std::nullopt, std::nullopt, entry);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsHumidity);

    entry.algorithm = "epa_2021";
    partial = correction_config(entry, std::nullopt, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsPm25);
  }

  SECTION("linear correction rejects PM-only flag") {
    CorrectionEntry entry = custom_linear(0.0, 1.0);
    entry.slr->use_epa2021 = false;
    const LocalServerConfig partial = correction_config(std::nullopt, entry, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsTemp);
  }

  SECTION("unsupported algorithms are rejected per target") {
    CorrectionEntry entry{};
    entry.algorithm = "slr";
    LocalServerConfig partial = correction_config(entry, std::nullopt, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsPm25);
    partial = correction_config(std::nullopt, entry, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsTemp);
    partial = correction_config(std::nullopt, std::nullopt, entry);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsHumidity);
  }

  SECTION("non-finite and float-overflow coefficients are rejected") {
    LocalServerConfig partial =
        correction_config(custom_pm25(std::numeric_limits<double>::infinity(), 1.0, false),
                          std::nullopt, std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsPm25);
    partial = correction_config(
        std::nullopt,
        custom_linear(0.0, static_cast<double>(std::numeric_limits<float>::max()) * 2.0),
        std::nullopt);
    require_status(fixture.service->submit_config(partial), ConfigSubmitStatus::InvalidValue,
                   ConfigFieldId::CorrectionsTemp);
  }
}

TEST_CASE("Go local API resets canonical correction values") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);

  CorrectionEntry none{};
  none.algorithm = "none";
  require_status(fixture.service->submit_config(correction_config(none, none, none)),
                 ConfigSubmitStatus::Accepted);
  LocalApiRequest request = fixture.receive_request();
  CHECK(request.config.corrections.pm25.algorithm == Pm25CorrectionAlgorithm::None);
  CHECK(request.config.corrections.pm25.scaling_factor == 1.0f);
  CHECK(request.config.corrections.pm25.intercept == 0.0f);
  CHECK_FALSE(request.config.corrections.pm25.use_epa2021);
  CHECK(request.config.corrections.temperature.algorithm == LinearCorrectionAlgorithm::None);
  CHECK(request.config.corrections.temperature.scaling_factor == 1.0f);
  CHECK(request.config.corrections.temperature.intercept == 0.0f);

  CorrectionEntry epa{};
  epa.algorithm = "epa_2021";
  require_status(fixture.service->submit_config(correction_config(epa, std::nullopt, std::nullopt)),
                 ConfigSubmitStatus::Accepted);
  request = fixture.receive_request();
  CHECK(request.config.corrections.pm25.algorithm == Pm25CorrectionAlgorithm::Epa2021);
  CHECK(request.config.corrections.pm25.scaling_factor == 1.0f);
  CHECK(request.config.corrections.pm25.intercept == 0.0f);
  CHECK_FALSE(request.config.corrections.pm25.use_epa2021);
}

TEST_CASE("Go local API FIFO preserves order and enforces four-entry capacity") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  const char *values[] = {"ugm3", "us-aqi", "ugm3", "us-aqi"};
  for (const char *value : values) {
    require_status(fixture.service->submit_config(pm_standard_config(value)),
                   ConfigSubmitStatus::Accepted);
  }
  require_status(fixture.service->submit_config(pm_standard_config("ugm3")),
                 ConfigSubmitStatus::Busy);
  LocalServerConfig invalid{};
  invalid.pm_standard = "invalid";
  require_status(fixture.service->submit_config(invalid), ConfigSubmitStatus::InvalidValue,
                 ConfigFieldId::PmStandard);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 4);

  for (size_t i = 0; i < 4; ++i) {
    const LocalApiRequest request = fixture.receive_request();
    CHECK(request.config.pm_use_usaqi == (i % 2 == 1));
  }
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 0);
}

TEST_CASE("Go local API FIFO wraps and preserves mixed request order") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  require_status(fixture.service->submit_config(pm_standard_config("ugm3")),
                 ConfigSubmitStatus::Accepted);
  require_status(fixture.service->submit_config(pm_standard_config("us-aqi")),
                 ConfigSubmitStatus::Accepted);
  require_status(fixture.service->submit_config(pm_standard_config("ugm3")),
                 ConfigSubmitStatus::Accepted);
  fixture.receive_request();
  fixture.receive_request();

  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Dispatched);
  require_status(fixture.service->submit_config(pm_standard_config("us-aqi")),
                 ConfigSubmitStatus::Accepted);

  CHECK(fixture.receive_request().kind == LocalApiRequestKind::Config);
  CHECK(fixture.receive_request().kind == LocalApiRequestKind::Action);
  CHECK(fixture.receive_request().kind == LocalApiRequestKind::Config);
}

TEST_CASE("Go local API rolls back local admission when central queue rejects") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  fixture.rtos.reject_queue_send = true;

  require_status(fixture.service->submit_config(pm_standard_config("us-aqi")),
                 ConfigSubmitStatus::Busy);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 0);

  fixture.rtos.reject_queue_send = false;
  require_status(fixture.service->submit_config(pm_standard_config("us-aqi")),
                 ConfigSubmitStatus::Accepted);
  CHECK(fixture.receive_request().config.pm_use_usaqi);
}

TEST_CASE("Go local API queue clear invalidates stale events") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  require_status(fixture.service->submit_config(pm_standard_config("ugm3")),
                 ConfigSubmitStatus::Accepted);
  const uint32_t old_epoch = fixture.service->queue_epoch();

  CHECK(fixture.service->clear_requests() == 1);
  CHECK(fixture.service->queue_epoch() == old_epoch + 1);
  CHECK(fixture.service->access() == ConfigAccess::ReadWrite);
  require_status(fixture.service->submit_config(pm_standard_config("us-aqi")),
                 ConfigSubmitStatus::Accepted);
  const Event stale = fixture.receive_event();
  const Event current = fixture.receive_event();
  CHECK(stale.local_api_epoch == old_epoch);
  CHECK(current.local_api_epoch == old_epoch + 1);

  LocalApiRequest request{};
  CHECK_FALSE(fixture.service->pop_request(stale.local_api_epoch, request));
  REQUIRE(fixture.service->pop_request(current.local_api_epoch, request));
  CHECK(request.config.pm_use_usaqi);
  CHECK(fixture.service->clear_requests() == 0);
  CHECK(fixture.service->queue_epoch() == old_epoch + 2);
}

TEST_CASE("Go local API rejects admission that spans a queue generation") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  const uint32_t old_epoch = fixture.service->queue_epoch();
  fixture.service->clear_requests();

  GoConfigUpdate update{};
  update.update_mask = field_mask(GoConfigField::PmStandard);
  update.pm_use_usaqi = true;
  require_status(GoLocalApiServiceTestAccess::admit_config(*fixture.service, update, old_epoch),
                 ConfigSubmitStatus::Busy);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 0);

  update = GoConfigUpdate{};
  require_status(GoLocalApiServiceTestAccess::admit_config(*fixture.service, update, old_epoch),
                 ConfigSubmitStatus::Accepted);
}

TEST_CASE("Go local API rechecks access and source during admission") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  GoConfigUpdate update{};
  update.update_mask = field_mask(GoConfigField::TemperatureUnit);
  update.use_fahrenheit = true;
  const uint32_t epoch = fixture.service->queue_epoch();

  fixture.service->set_access(ConfigAccess::ReadOnly);
  require_status(GoLocalApiServiceTestAccess::admit_config(*fixture.service, update, epoch),
                 ConfigSubmitStatus::Forbidden);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 0);

  fixture.service->set_access(ConfigAccess::ReadWrite);
  GoSettings settings{};
  settings.configuration_control = ConfigurationControl::Cloud;
  fixture.service->publish_config_snapshot(settings);
  require_status(GoLocalApiServiceTestAccess::admit_config(*fixture.service, update, epoch),
                 ConfigSubmitStatus::Forbidden);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 0);

  Event event{};
  CHECK_FALSE(RTOS::queue_receive(fixture.event_queue, &event, 0));
}

TEST_CASE("Go local API fails admission safely without a central queue") {
  GoLocalApiService::Config config{};
  config.serial_number = TEST_SERIAL;
  config.firmware_version = TEST_FIRMWARE;
  GoLocalApiService service(nullptr, config);
  CHECK_FALSE(service.is_valid());

  service.set_access(ConfigAccess::ReadWrite);
  require_status(service.submit_config(LocalServerConfig{}), ConfigSubmitStatus::Accepted);
  require_status(service.submit_config(pm_standard_config("us-aqi")), ConfigSubmitStatus::Busy);
  CHECK(service.trigger(ActionId::CalibrateCo2).status == ActionStatus::Busy);
  CHECK(GoLocalApiServiceTestAccess::request_count(service) == 0);
}

TEST_CASE("Go local API access changes do not implicitly clear admitted work") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  require_status(fixture.service->submit_config(pm_standard_config("us-aqi")),
                 ConfigSubmitStatus::Accepted);
  fixture.service->set_access(ConfigAccess::ReadOnly);

  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 1);
  CHECK(fixture.receive_request().config.pm_use_usaqi);
}

TEST_CASE("Go local API action access precedes catalog support") {
  Fixture fixture;
  CHECK(fixture.service->trigger(ActionId::TestLeds).status == ActionStatus::Rejected);
  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Rejected);

  fixture.service->set_access(ConfigAccess::ReadOnly);
  CHECK(fixture.service->trigger(ActionId::TestLeds).status == ActionStatus::Rejected);
  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Rejected);

  fixture.service->set_access(ConfigAccess::ReadWrite);
  CHECK(fixture.service->trigger(ActionId::TestLeds).status == ActionStatus::NotSupported);
  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Dispatched);
}

TEST_CASE("Go local API queues calibration actions independently") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);

  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Dispatched);
  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Dispatched);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 2);

  for (size_t i = 0; i < 2; ++i) {
    const LocalApiRequest request = fixture.receive_request();
    CHECK(request.kind == LocalApiRequestKind::Action);
    CHECK(request.action == ActionId::CalibrateCo2);
  }
}

TEST_CASE("Go local API rolls back action after admission failure") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  fixture.rtos.reject_queue_send = true;

  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Busy);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 0);

  fixture.rtos.reject_queue_send = false;
  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Dispatched);
  CHECK(GoLocalApiServiceTestAccess::request_count(*fixture.service) == 1);
}

TEST_CASE("Go local API clears queued calibration actions") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Dispatched);
  CHECK(fixture.service->clear_requests() == 1);
  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Dispatched);
}

TEST_CASE("Go local API action reports busy only while the request queue is full") {
  Fixture fixture;
  fixture.service->set_access(ConfigAccess::ReadWrite);
  for (size_t i = 0; i < LOCAL_API_REQUEST_QUEUE_DEPTH; ++i) {
    require_status(fixture.service->submit_config(pm_standard_config("ugm3")),
                   ConfigSubmitStatus::Accepted);
  }

  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Busy);
  fixture.receive_request();
  CHECK(fixture.service->trigger(ActionId::CalibrateCo2).status == ActionStatus::Dispatched);
}
