/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#ifndef AG_LOCAL_SERVER_LOCAL_CONFIG_H
#define AG_LOCAL_SERVER_LOCAL_CONFIG_H

#include <optional>
#include <string>

// One per-measure correction entry, mirroring the legacy cloud shape. Parsing
// preserves coefficient presence so products can reject an incomplete SLR
// semantically. GET serialization requires both coefficients for every
// non-null SLR. `use_epa2021` is valid only for pm25.
struct SlrParams {
  std::optional<double> intercept;      // "intercept"
  std::optional<double> scaling_factor; // "scalingFactor"
  std::optional<bool> use_epa2021;      // "useEpa2021" (pm25 only)
};

struct CorrectionEntry {
  std::string algorithm;        // "correctionAlgorithm" ("none" disables)
  std::optional<SlrParams> slr; // "slr" (null -> nullopt)
};

// Nested object; the single exception to the flat schema. Inner keys use v1
// measure vocabulary (pm25 / temp / humidity), not legacy (pm02 / atmp / rhum).
struct Corrections {
  std::optional<CorrectionEntry> pm25;     // "pm25"
  std::optional<CorrectionEntry> temp;     // "temp"
  std::optional<CorrectionEntry> humidity; // "humidity"
};

// Flat configuration schema for GET / PUT /api/v1/config. Every field is
// optional both on the wire and here: a device emits only the fields its
// model supports (GET) and applies only the present supported fields (PUT).
// Fields are named by function, not by product. The component owns this
// catalog as a union of known fields; adding a future field (including a
// product-specific one) is a non-breaking addition of one optional field.
//
// C++ members stay snake_case (firmware style); the camelCase wire key is the
// trailing comment. The serialize / parse layer is the only place the two
// vocabularies meet.
struct LocalServerConfig {
  std::optional<std::string> country;               // "country"
  std::optional<std::string> pm_standard;           // "pmStandard"
  std::optional<std::string> temperature_unit;      // "temperatureUnit"
  std::optional<bool> post_data_to_cloud;           // "postDataToCloud"
  std::optional<bool> cloud_connection;             // "cloudConnection"
  std::optional<std::string> configuration_control; // "configurationControl"
  std::optional<int> co2_abc_days;                  // "co2AbcDays"
  std::optional<int> tvoc_learning_offset;          // "tvocLearningOffset"
  std::optional<int> nox_learning_offset;           // "noxLearningOffset"
  std::optional<std::string> led_mode;              // "ledMode"
  std::optional<int> led_bar_brightness;            // "ledBarBrightness"
  std::optional<int> display_brightness;            // "displayBrightness"
  std::optional<std::string> mqtt_broker_url;       // "mqttBrokerUrl"
  std::optional<std::string> http_domain;           // "httpDomain"
  std::optional<Corrections> corrections;           // "corrections"
  // Product-specific fields (for example buzzer_enabled, gps_interval_s) are
  // added here as flat optional fields when a product exposes them over HTTP.
};

#endif // AG_LOCAL_SERVER_LOCAL_CONFIG_H
