/**
 * AirGradient
 * https://airgradient.com
 *
 * CC BY-SA 4.0 Attribution-ShareAlike 4.0 International License
 */

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <trompeloeil.hpp>
#include <trompeloeil/mock.hpp>

#include "hal/battery_mgmt_sensor.h"
#include "hal/co2_sensor.h"
#include "hal/o3_no2_sensor.h"
#include "hal/pm_sensor.h"
#include "hal/temp_hum_sensor.h"
#include "hal/tvoc_nox_sensor.h"
#include "measures_types.h"
#include "rtos.h"
#include "services/sensor_manager.h"

using Catch::Matchers::WithinAbs;

#define EXPECT_READ(mock, data, status)                                        \
  REQUIRE_CALL((mock), read(trompeloeil::_))                                   \
      .LR_SIDE_EFFECT(_1 = (data))                                             \
      .RETURN(status)

#define EXPECT_FAILURE(mock)                                                   \
  REQUIRE_CALL(mock, read(trompeloeil::_)).RETURN(false)

class MockTempHumSensor : public trompeloeil::mock_interface<TempHumSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
};

class MockBatteryMgmtSensor
    : public trompeloeil::mock_interface<BatteryMgmtSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
};

class MockCO2Sensor : public trompeloeil::mock_interface<CO2Sensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
};

class MockO3No2Sensor : public trompeloeil::mock_interface<O3No2Sensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
};

class MockPMSensor : public trompeloeil::mock_interface<PMSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
  IMPLEMENT_CONST_MOCK0(supports_temp_hum);
  IMPLEMENT_MOCK0(temp_hum_data);
};

class MockTVOCNOxSensor : public trompeloeil::mock_interface<TVOCNOxSensor> {
public:
  IMPLEMENT_MOCK0(init);
  IMPLEMENT_MOCK1(read);
};

class MockRTOS : public trompeloeil::mock_interface<RTOS> {
public:
  IMPLEMENT_MOCK1(delay_ms_impl);
  IMPLEMENT_MOCK0(get_time_ms_impl);
};

TEST_CASE("Averaging", "[SensorManager]") {
  MockTempHumSensor mock_tempHum;
  MockCO2Sensor mock_co2;
  MockPMSensor mock_pm_a;
  MockPMSensor mock_pm_b;
  MockTVOCNOxSensor mock_tvoc_nox;
  MockBatteryMgmtSensor mock_battery;
  MockO3No2Sensor mock_o3no2;

  // Allow supports_temp_hum() calls on PM sensors (used internally by
  // SensorManager)
  ALLOW_CALL(mock_pm_a, supports_temp_hum()).RETURN(false);
  ALLOW_CALL(mock_pm_b, supports_temp_hum()).RETURN(false);

  Sensors sensors;
  sensors.temp_hum = &mock_tempHum;
  sensors.co2 = &mock_co2;
  sensors.pms_a = &mock_pm_a;
  sensors.pms_b = &mock_pm_b;
  sensors.tvoc_nox = &mock_tvoc_nox;
  sensors.battery_mgmt = &mock_battery;
  sensors.o3_no2 = &mock_o3no2;

  SensorManager sensor_manager(sensors);

  // Setup RTOS mock for all sections
  MockRTOS mock_rtos;
  RTOS::set_instance(&mock_rtos);
  trompeloeil::sequence seq;

  SECTION("Handles null sensor") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // NOTE: This will crash if there's no nullptr check inside start_measures
    Sensors xsensors;
    xsensors.temp_hum = nullptr;
    xsensors.co2 = nullptr;
    xsensors.pms_a = nullptr;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.battery_mgmt = nullptr;
    xsensors.o3_no2 = nullptr;
    SensorManager xsensor_manager(xsensors);
    xsensor_manager.start_measures(1);
  }

  SECTION("1 Iteration all success") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    EXPECT_READ(mock_tempHum, (TempHumData{10.0f, 50.0f}), true);
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_pm_a,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f,
                        2.5f, 5.0f, 10.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{101.0f, 102.5f, 110.0f, 101.1f, 102.6f, 110.1f, 100.3f,
                        100.5f, 101.0f, 102.5f, 105.0f, 110.0f}),
                true);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{100, 200, 50, 75}), true);
    EXPECT_READ(mock_battery, (BatteryMgmtData{3.7f, 5.0f}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{0.5f, 0.6f, 0.7f, 0.8f, 25.0f}), true);

    auto result = sensor_manager.start_measures(1);

    // TempHum assertions - temp_hum_a from dedicated sensor, temp_hum_b should
    // be invalid
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(50.0f, 0.001f));
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // CO2 assertions
    REQUIRE(result.co2.co2 == 400);

    // PM assertions - all 12 fields
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_25, WithinAbs(2.5f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_10, WithinAbs(10.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_01_sp, WithinAbs(1.1f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_25_sp, WithinAbs(2.6f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_10_sp, WithinAbs(10.1f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_03_pc, WithinAbs(0.3f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_05_pc, WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_01_pc, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_25_pc, WithinAbs(2.5f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_5_pc, WithinAbs(5.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_10_pc, WithinAbs(10.0f, 0.001f));

    // PM_B assertions - all 12 fields
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(101.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_25, WithinAbs(102.5f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_10, WithinAbs(110.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_01_sp, WithinAbs(101.1f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_25_sp, WithinAbs(102.6f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_10_sp, WithinAbs(110.1f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_03_pc, WithinAbs(100.3f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_05_pc, WithinAbs(100.5f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_01_pc, WithinAbs(101.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_25_pc, WithinAbs(102.5f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_5_pc, WithinAbs(105.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_10_pc, WithinAbs(110.0f, 0.001f));

    // TVOCNOx assertions - all 4 fields
    REQUIRE(result.tvoc_nox.tvoc_index == 100);
    REQUIRE(result.tvoc_nox.tvoc_raw == 200);
    REQUIRE(result.tvoc_nox.nox_index == 50);
    REQUIRE(result.tvoc_nox.nox_raw == 75);

    // Battery assertions
    REQUIRE_THAT(result.power.volt_battery, WithinAbs(3.7f, 0.001f));
    REQUIRE_THAT(result.power.volt_charging, WithinAbs(5.0f, 0.001f));

    // O3No2 assertions - all 5 fields
    REQUIRE_THAT(result.electrode.o3_we, WithinAbs(0.5f, 0.001f));
    REQUIRE_THAT(result.electrode.o3_ae, WithinAbs(0.6f, 0.001f));
    REQUIRE_THAT(result.electrode.no2_we, WithinAbs(0.7f, 0.001f));
    REQUIRE_THAT(result.electrode.no2_ae, WithinAbs(0.8f, 0.001f));
    REQUIRE_THAT(result.electrode.afe_temp, WithinAbs(25.0f, 0.001f));
  }

  SECTION("5 iteration all success") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // TempHum - 5 iterations
    EXPECT_READ(mock_tempHum, (TempHumData{10.0f, 20.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{20.0f, 30.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{30.0f, 40.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{40.0f, 50.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{50.0f, 60.0f}), true);

    // CO2 - 5 iterations
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_READ(mock_co2, (CO2Data{500}), true);
    EXPECT_READ(mock_co2, (CO2Data{600}), true);
    EXPECT_READ(mock_co2, (CO2Data{700}), true);
    EXPECT_READ(mock_co2, (CO2Data{800}), true);

    // PM - 5 iterations
    EXPECT_READ(mock_pm_a,
                (PMData{1.0f, 2.0f, 10.0f, 1.0f, 2.0f, 10.0f, 1.0f, 2.0f, 3.0f,
                        4.0f, 5.0f, 6.0f}),
                true);
    EXPECT_READ(mock_pm_a,
                (PMData{2.0f, 4.0f, 20.0f, 2.0f, 4.0f, 20.0f, 2.0f, 4.0f, 6.0f,
                        8.0f, 10.0f, 12.0f}),
                true);
    EXPECT_READ(mock_pm_a,
                (PMData{3.0f, 6.0f, 30.0f, 3.0f, 6.0f, 30.0f, 3.0f, 6.0f, 9.0f,
                        12.0f, 15.0f, 18.0f}),
                true);
    EXPECT_READ(mock_pm_a,
                (PMData{4.0f, 8.0f, 40.0f, 4.0f, 8.0f, 40.0f, 4.0f, 8.0f, 12.0f,
                        16.0f, 20.0f, 24.0f}),
                true);
    EXPECT_READ(mock_pm_a,
                (PMData{5.0f, 10.0f, 50.0f, 5.0f, 10.0f, 50.0f, 5.0f, 10.0f,
                        15.0f, 20.0f, 25.0f, 30.0f}),
                true);

    // PM_B - 5 iterations
    EXPECT_READ(mock_pm_b,
                (PMData{10.0f, 20.0f, 100.0f, 10.0f, 20.0f, 100.0f, 10.0f,
                        20.0f, 30.0f, 40.0f, 50.0f, 60.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{20.0f, 40.0f, 200.0f, 20.0f, 40.0f, 200.0f, 20.0f,
                        40.0f, 60.0f, 80.0f, 100.0f, 120.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{30.0f, 60.0f, 300.0f, 30.0f, 60.0f, 300.0f, 30.0f,
                        60.0f, 90.0f, 120.0f, 150.0f, 180.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{40.0f, 80.0f, 400.0f, 40.0f, 80.0f, 400.0f, 40.0f,
                        80.0f, 120.0f, 160.0f, 200.0f, 240.0f}),
                true);
    EXPECT_READ(mock_pm_b,
                (PMData{50.0f, 100.0f, 500.0f, 50.0f, 100.0f, 500.0f, 50.0f,
                        100.0f, 150.0f, 200.0f, 250.0f, 300.0f}),
                true);

    // TVOCNOx - 5 iterations
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{100, 200, 50, 100}), true);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{200, 300, 100, 150}), true);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{300, 400, 150, 200}), true);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{400, 500, 200, 250}), true);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{500, 600, 250, 300}), true);

    // Battery - 5 iterations
    EXPECT_READ(mock_battery, (BatteryMgmtData{3.5f, 4.5f}), true);
    EXPECT_READ(mock_battery, (BatteryMgmtData{3.6f, 4.6f}), true);
    EXPECT_READ(mock_battery, (BatteryMgmtData{3.7f, 4.7f}), true);
    EXPECT_READ(mock_battery, (BatteryMgmtData{3.8f, 4.8f}), true);
    EXPECT_READ(mock_battery, (BatteryMgmtData{3.9f, 4.9f}), true);

    // O3No2 - 5 iterations
    EXPECT_READ(mock_o3no2, (O3No2Data{0.5f, 0.6f, 0.7f, 0.8f, 20.0f}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{1.0f, 1.1f, 1.2f, 1.3f, 22.0f}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{1.5f, 1.6f, 1.7f, 1.8f, 24.0f}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{2.0f, 2.1f, 2.2f, 2.3f, 26.0f}), true);
    EXPECT_READ(mock_o3no2, (O3No2Data{2.5f, 2.6f, 2.7f, 2.8f, 28.0f}), true);

    auto result = sensor_manager.start_measures(5);

    // TempHum assertions - temp_hum_a from dedicated sensor, temp_hum_b should
    // be invalid (average of 10,20,30,40,50 = 30; 20,30,40,50,60 = 40)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(30.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(40.0f, 0.001f));
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // CO2 assertions (average of 400,500,600,700,800 = 600)
    REQUIRE(result.co2.co2 == 600);

    // PM assertions (average of each field)
    REQUIRE_THAT(result.pm_a.pm_01,
                 WithinAbs(3.0f, 0.001f)); // avg of 1,2,3,4,5
    REQUIRE_THAT(result.pm_a.pm_25,
                 WithinAbs(6.0f, 0.001f)); // avg of 2,4,6,8,10
    REQUIRE_THAT(result.pm_a.pm_10,
                 WithinAbs(30.0f, 0.001f)); // avg of 10,20,30,40,50
    REQUIRE_THAT(result.pm_a.pm_01_sp,
                 WithinAbs(3.0f, 0.001f)); // avg of 1,2,3,4,5
    REQUIRE_THAT(result.pm_a.pm_25_sp,
                 WithinAbs(6.0f, 0.001f)); // avg of 2,4,6,8,10
    REQUIRE_THAT(result.pm_a.pm_10_sp,
                 WithinAbs(30.0f, 0.001f)); // avg of 10,20,30,40,50
    REQUIRE_THAT(result.pm_a.pm_03_pc,
                 WithinAbs(3.0f, 0.001f)); // avg of 1,2,3,4,5
    REQUIRE_THAT(result.pm_a.pm_05_pc,
                 WithinAbs(6.0f, 0.001f)); // avg of 2,4,6,8,10
    REQUIRE_THAT(result.pm_a.pm_01_pc,
                 WithinAbs(9.0f, 0.001f)); // avg of 3,6,9,12,15
    REQUIRE_THAT(result.pm_a.pm_25_pc,
                 WithinAbs(12.0f, 0.001f)); // avg of 4,8,12,16,20
    REQUIRE_THAT(result.pm_a.pm_5_pc,
                 WithinAbs(15.0f, 0.001f)); // avg of 5,10,15,20,25
    REQUIRE_THAT(result.pm_a.pm_10_pc,
                 WithinAbs(18.0f, 0.001f)); // avg of 6,12,18,24,30

    // PM_B assertions (average of each field)
    REQUIRE_THAT(result.pm_b.pm_01,
                 WithinAbs(30.0f, 0.001f)); // avg of 10,20,30,40,50
    REQUIRE_THAT(result.pm_b.pm_25,
                 WithinAbs(60.0f, 0.001f)); // avg of 20,40,60,80,100
    REQUIRE_THAT(result.pm_b.pm_10,
                 WithinAbs(300.0f, 0.001f)); // avg of 100,200,300,400,500
    REQUIRE_THAT(result.pm_b.pm_01_sp,
                 WithinAbs(30.0f, 0.001f)); // avg of 10,20,30,40,50
    REQUIRE_THAT(result.pm_b.pm_25_sp,
                 WithinAbs(60.0f, 0.001f)); // avg of 20,40,60,80,100
    REQUIRE_THAT(result.pm_b.pm_10_sp,
                 WithinAbs(300.0f, 0.001f)); // avg of 100,200,300,400,500
    REQUIRE_THAT(result.pm_b.pm_03_pc,
                 WithinAbs(30.0f, 0.001f)); // avg of 10,20,30,40,50
    REQUIRE_THAT(result.pm_b.pm_05_pc,
                 WithinAbs(60.0f, 0.001f)); // avg of 20,40,60,80,100
    REQUIRE_THAT(result.pm_b.pm_01_pc,
                 WithinAbs(90.0f, 0.001f)); // avg of 30,60,90,120,150
    REQUIRE_THAT(result.pm_b.pm_25_pc,
                 WithinAbs(120.0f, 0.001f)); // avg of 40,80,120,160,200
    REQUIRE_THAT(result.pm_b.pm_5_pc,
                 WithinAbs(150.0f, 0.001f)); // avg of 50,100,150,200,250
    REQUIRE_THAT(result.pm_b.pm_10_pc,
                 WithinAbs(180.0f, 0.001f)); // avg of 60,120,180,240,300

    // TVOCNOx assertions (average of each field)
    REQUIRE(result.tvoc_nox.tvoc_index == 300); // avg of 100,200,300,400,500
    REQUIRE(result.tvoc_nox.tvoc_raw == 400);   // avg of 200,300,400,500,600
    REQUIRE(result.tvoc_nox.nox_index == 150);  // avg of 50,100,150,200,250
    REQUIRE(result.tvoc_nox.nox_raw == 200);    // avg of 100,150,200,250,300

    // Battery assertions (average of each field)
    REQUIRE_THAT(result.power.volt_battery,
                 WithinAbs(3.7f, 0.001f)); // avg of 3.5,3.6,3.7,3.8,3.9
    REQUIRE_THAT(result.power.volt_charging,
                 WithinAbs(4.7f, 0.001f)); // avg of 4.5,4.6,4.7,4.8,4.9

    // O3No2 assertions (average of each field)
    REQUIRE_THAT(result.electrode.o3_we,
                 WithinAbs(1.5f, 0.001f)); // avg of 0.5,1.0,1.5,2.0,2.5
    REQUIRE_THAT(result.electrode.o3_ae,
                 WithinAbs(1.6f, 0.001f)); // avg of 0.6,1.1,1.6,2.1,2.6
    REQUIRE_THAT(result.electrode.no2_we,
                 WithinAbs(1.7f, 0.001f)); // avg of 0.7,1.2,1.7,2.2,2.7
    REQUIRE_THAT(result.electrode.no2_ae,
                 WithinAbs(1.8f, 0.001f)); // avg of 0.8,1.3,1.8,2.3,2.8
    REQUIRE_THAT(result.electrode.afe_temp,
                 WithinAbs(24.0f, 0.001f)); // avg of 20,22,24,26,28
  }

  SECTION("3 iteration 1 failed") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // TempHum - 3 iterations (1 fails)
    EXPECT_READ(mock_tempHum, (TempHumData{10.0f, 20.0f}), true);
    EXPECT_FAILURE(mock_tempHum);
    EXPECT_READ(mock_tempHum, (TempHumData{40.0f, 40.0f}), true);

    // CO2 - 3 iterations (1 fails)
    EXPECT_READ(mock_co2, (CO2Data{400}), true);
    EXPECT_FAILURE(mock_co2);
    EXPECT_READ(mock_co2, (CO2Data{600}), true);

    // PM - 3 iterations (1 fails)
    EXPECT_READ(mock_pm_a,
                (PMData{1.0f, 2.0f, 10.0f, 1.0f, 2.0f, 10.0f, 1.0f, 2.0f, 3.0f,
                        4.0f, 5.0f, 6.0f}),
                true);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_READ(mock_pm_a,
                (PMData{5.0f, 10.0f, 50.0f, 5.0f, 10.0f, 50.0f, 5.0f, 10.0f,
                        15.0f, 20.0f, 25.0f, 30.0f}),
                true);

    // PM_B - 3 iterations (1 fails)
    EXPECT_READ(mock_pm_b,
                (PMData{10.0f, 20.0f, 100.0f, 10.0f, 20.0f, 100.0f, 10.0f,
                        20.0f, 30.0f, 40.0f, 50.0f, 60.0f}),
                true);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_READ(mock_pm_b,
                (PMData{30.0f, 60.0f, 300.0f, 30.0f, 60.0f, 300.0f, 30.0f,
                        60.0f, 90.0f, 120.0f, 150.0f, 180.0f}),
                true);

    // TVOCNOx - 3 iterations (1 fails)
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{100, 200, 50, 100}), true);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_READ(mock_tvoc_nox, (TVOCNOxData{500, 600, 250, 300}), true);

    // Battery - 3 iterations (1 fails)
    EXPECT_READ(mock_battery, (BatteryMgmtData{3.5f, 4.5f}), true);
    EXPECT_FAILURE(mock_battery);
    EXPECT_READ(mock_battery, (BatteryMgmtData{3.9f, 4.9f}), true);

    // O3No2 - 3 iterations (1 fails)
    EXPECT_READ(mock_o3no2, (O3No2Data{0.5f, 0.6f, 0.7f, 0.8f, 20.0f}), true);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_READ(mock_o3no2, (O3No2Data{2.5f, 2.6f, 2.7f, 2.8f, 28.0f}), true);

    auto result = sensor_manager.start_measures(3);

    // TempHum assertions - temp_hum_a from dedicated sensor, temp_hum_b should
    // be invalid (average of 2 successful: (10+40)/2=25, (20+40)/2=30)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(30.0f, 0.001f));
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // CO2 assertions (average of 2 successful: (400+600)/2=500)
    REQUIRE(result.co2.co2 == 500);

    // PM assertions (average of 2 successful reads)
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(3.0f, 0.001f));     // (1+5)/2
    REQUIRE_THAT(result.pm_a.pm_25, WithinAbs(6.0f, 0.001f));     // (2+10)/2
    REQUIRE_THAT(result.pm_a.pm_10, WithinAbs(30.0f, 0.001f));    // (10+50)/2
    REQUIRE_THAT(result.pm_a.pm_01_sp, WithinAbs(3.0f, 0.001f));  // (1+5)/2
    REQUIRE_THAT(result.pm_a.pm_25_sp, WithinAbs(6.0f, 0.001f));  // (2+10)/2
    REQUIRE_THAT(result.pm_a.pm_10_sp, WithinAbs(30.0f, 0.001f)); // (10+50)/2
    REQUIRE_THAT(result.pm_a.pm_03_pc, WithinAbs(3.0f, 0.001f));  // (1+5)/2
    REQUIRE_THAT(result.pm_a.pm_05_pc, WithinAbs(6.0f, 0.001f));  // (2+10)/2
    REQUIRE_THAT(result.pm_a.pm_01_pc, WithinAbs(9.0f, 0.001f));  // (3+15)/2
    REQUIRE_THAT(result.pm_a.pm_25_pc, WithinAbs(12.0f, 0.001f)); // (4+20)/2
    REQUIRE_THAT(result.pm_a.pm_5_pc, WithinAbs(15.0f, 0.001f));  // (5+25)/2
    REQUIRE_THAT(result.pm_a.pm_10_pc, WithinAbs(18.0f, 0.001f)); // (6+30)/2

    // PM_B assertions (average of 2 successful reads)
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(20.0f, 0.001f));    // (10+30)/2
    REQUIRE_THAT(result.pm_b.pm_25, WithinAbs(40.0f, 0.001f));    // (20+60)/2
    REQUIRE_THAT(result.pm_b.pm_10, WithinAbs(200.0f, 0.001f));   // (100+300)/2
    REQUIRE_THAT(result.pm_b.pm_01_sp, WithinAbs(20.0f, 0.001f)); // (10+30)/2
    REQUIRE_THAT(result.pm_b.pm_25_sp, WithinAbs(40.0f, 0.001f)); // (20+60)/2
    REQUIRE_THAT(result.pm_b.pm_10_sp,
                 WithinAbs(200.0f, 0.001f));                      // (100+300)/2
    REQUIRE_THAT(result.pm_b.pm_03_pc, WithinAbs(20.0f, 0.001f)); // (10+30)/2
    REQUIRE_THAT(result.pm_b.pm_05_pc, WithinAbs(40.0f, 0.001f)); // (20+60)/2
    REQUIRE_THAT(result.pm_b.pm_01_pc, WithinAbs(60.0f, 0.001f)); // (30+90)/2
    REQUIRE_THAT(result.pm_b.pm_25_pc, WithinAbs(80.0f, 0.001f)); // (40+120)/2
    REQUIRE_THAT(result.pm_b.pm_5_pc, WithinAbs(100.0f, 0.001f)); // (50+150)/2
    REQUIRE_THAT(result.pm_b.pm_10_pc, WithinAbs(120.0f, 0.001f)); // (60+180)/2

    // TVOCNOx assertions (average of 2 successful reads)
    REQUIRE(result.tvoc_nox.tvoc_index == 300); // (100+500)/2
    REQUIRE(result.tvoc_nox.tvoc_raw == 400);   // (200+600)/2
    REQUIRE(result.tvoc_nox.nox_index == 150);  // (50+250)/2
    REQUIRE(result.tvoc_nox.nox_raw == 200);    // (100+300)/2

    // Battery assertions (average of 2 successful reads)
    REQUIRE_THAT(result.power.volt_battery,
                 WithinAbs(3.7f, 0.001f)); // (3.5+3.9)/2
    REQUIRE_THAT(result.power.volt_charging,
                 WithinAbs(4.7f, 0.001f)); // (4.5+4.9)/2

    // O3No2 assertions (average of 2 successful reads)
    REQUIRE_THAT(result.electrode.o3_we,
                 WithinAbs(1.5f, 0.001f)); // (0.5+2.5)/2
    REQUIRE_THAT(result.electrode.o3_ae,
                 WithinAbs(1.6f, 0.001f)); // (0.6+2.6)/2
    REQUIRE_THAT(result.electrode.no2_we,
                 WithinAbs(1.7f, 0.001f)); // (0.7+2.7)/2
    REQUIRE_THAT(result.electrode.no2_ae,
                 WithinAbs(1.8f, 0.001f)); // (0.8+2.8)/2
    REQUIRE_THAT(result.electrode.afe_temp,
                 WithinAbs(24.0f, 0.001f)); // (20+28)/2
  }

  SECTION("No valid value") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // All sensors fail to read
    EXPECT_FAILURE(mock_tempHum);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_battery);
    EXPECT_FAILURE(mock_o3no2);

    auto result = sensor_manager.start_measures(1);

    // TempHum assertions - should be invalid
    REQUIRE(result.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // CO2 assertions - should be invalid
    REQUIRE(result.co2.co2 == MeasuresInvalid::CO2);

    // PM assertions - all fields should be invalid
    REQUIRE(result.pm_a.pm_01 == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_25 == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_10 == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_01_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_25_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_10_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_03_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_05_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_01_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_25_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_5_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_a.pm_10_pc == MeasuresInvalid::PM);

    // PM_B assertions - all fields should be invalid
    REQUIRE(result.pm_b.pm_01 == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_25 == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_10 == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_01_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_25_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_10_sp == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_03_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_05_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_01_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_25_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_5_pc == MeasuresInvalid::PM);
    REQUIRE(result.pm_b.pm_10_pc == MeasuresInvalid::PM);

    // TVOCNOx assertions - should be invalid
    REQUIRE(result.tvoc_nox.tvoc_index == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.tvoc_raw == MeasuresInvalid::TVOC);
    REQUIRE(result.tvoc_nox.nox_index == MeasuresInvalid::NOX);
    REQUIRE(result.tvoc_nox.nox_raw == MeasuresInvalid::NOX);

    // Battery assertions - should be invalid
    REQUIRE(result.power.volt_battery == MeasuresInvalid::VOLT);
    REQUIRE(result.power.volt_charging == MeasuresInvalid::VOLT);

    // O3No2 assertions - should be invalid
    REQUIRE(result.electrode.o3_we == MeasuresInvalid::VOLT);
    REQUIRE(result.electrode.o3_ae == MeasuresInvalid::VOLT);
    REQUIRE(result.electrode.no2_we == MeasuresInvalid::VOLT);
    REQUIRE(result.electrode.no2_ae == MeasuresInvalid::VOLT);
    REQUIRE(result.electrode.afe_temp == MeasuresInvalid::VOLT);
  }

  SECTION("Negative temperature handling") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Test valid negative temperatures (within range: -40°C to 125°C)
    EXPECT_READ(mock_tempHum, (TempHumData{-20.0f, 50.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{-10.0f, 60.0f}), true);
    EXPECT_READ(mock_tempHum, (TempHumData{-30.0f, 70.0f}), true);

    // Other sensors - just make them fail (Only testing temp hum)
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_battery);
    EXPECT_FAILURE(mock_battery);
    EXPECT_FAILURE(mock_battery);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);

    auto result = sensor_manager.start_measures(3);

    // Valid negative temperatures should be averaged correctly
    // Average: (-20 + -10 + -30) / 3 = -20°C
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(-20.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(60.0f, 0.001f));
  }

  SECTION("Invalid negative temperature") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Test invalid negative temperatures (below MIN_VALID_TEMP = -40°C)
    EXPECT_READ(mock_tempHum, (TempHumData{-50.0f, 50.0f}),
                true); // Too cold, invalid
    EXPECT_READ(mock_tempHum, (TempHumData{-60.0f, 60.0f}),
                true); // Too cold, invalid
    EXPECT_READ(mock_tempHum, (TempHumData{-100.0f, 70.0f}),
                true); // Too cold, invalid

    // Other sensors - just make them fail (Only testing temp hum)
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_battery);
    EXPECT_FAILURE(mock_battery);
    EXPECT_FAILURE(mock_battery);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);

    auto result = sensor_manager.start_measures(3);

    // temp_hum_a from dedicated sensor - all temperatures invalid, humidity
    // valid
    REQUIRE(result.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(60.0f, 0.001f));
    // temp_hum_b should be invalid (dedicated sensor present)
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);
  }

  SECTION("Mixed valid and invalid negative temperature") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Mix of valid and invalid negative temperatures
    EXPECT_READ(mock_tempHum, (TempHumData{-30.0f, 50.0f}), true); // Valid
    EXPECT_READ(mock_tempHum, (TempHumData{-50.0f, 60.0f}),
                true); // Invalid (too cold)
    EXPECT_READ(mock_tempHum, (TempHumData{-20.0f, 70.0f}), true); // Valid

    // Other sensors - just make them fail (Only testing temp hum)
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_co2);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_a);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_pm_b);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_tvoc_nox);
    EXPECT_FAILURE(mock_battery);
    EXPECT_FAILURE(mock_battery);
    EXPECT_FAILURE(mock_battery);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);
    EXPECT_FAILURE(mock_o3no2);

    auto result = sensor_manager.start_measures(3);

    // temp_hum_a from dedicated sensor - only valid temperatures averaged: (-30
    // + -20) / 2 = -25°C
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(-25.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(60.0f, 0.001f));
    // temp_hum_b should be invalid (dedicated sensor present)
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);
  }

  SECTION("Temperature and Humidity from PM sensor") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockPMSensor mock_pm_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr;
    xsensors.co2 = nullptr;
    xsensors.pms_a = &mock_pm_local;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.battery_mgmt = nullptr;
    xsensors.o3_no2 = nullptr;
    SensorManager xsensor_manager(xsensors);

    // PM sensor supports temp/hum and provides data
    REQUIRE_CALL(mock_pm_local, supports_temp_hum()).RETURN(true);
    REQUIRE_CALL(mock_pm_local, temp_hum_data())
        .RETURN(TempHumData{25.0f, 60.0f});

    // PM sensor also provides PM data
    EXPECT_READ(mock_pm_local,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f,
                        2.5f, 5.0f, 10.0f}),
                true);

    auto result = xsensor_manager.start_measures(1);

    // Verify temp_hum_a comes from pms_a (no dedicated sensor)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(60.0f, 0.001f));
    // temp_hum_b should be invalid (no pms_b)
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // Verify PM data is also present
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_25, WithinAbs(2.5f, 0.001f));
  }

  SECTION("PM sensor does not support Temperature and Humidity") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockPMSensor mock_pm_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr;
    xsensors.co2 = nullptr;
    xsensors.pms_a = &mock_pm_local;
    xsensors.pms_b = nullptr;
    xsensors.tvoc_nox = nullptr;
    xsensors.battery_mgmt = nullptr;
    xsensors.o3_no2 = nullptr;
    SensorManager xsensor_manager(xsensors);

    // PM sensor does NOT support temp/hum
    REQUIRE_CALL(mock_pm_local, supports_temp_hum()).RETURN(false);

    // PM sensor provides PM data only
    EXPECT_READ(mock_pm_local,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f,
                        2.5f, 5.0f, 10.0f}),
                true);

    auto result = xsensor_manager.start_measures(1);

    // Verify both temp/hum fields are invalid (no temp/hum source available)
    REQUIRE(result.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // Verify PM data is present
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_a.pm_25, WithinAbs(2.5f, 0.001f));
  }

  SECTION("Both PM sensors support temp/hum independently") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockPMSensor mock_pm_a_local;
    MockPMSensor mock_pm_b_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr; // No dedicated sensor
    xsensors.co2 = nullptr;
    xsensors.pms_a = &mock_pm_a_local;
    xsensors.pms_b = &mock_pm_b_local;
    xsensors.tvoc_nox = nullptr;
    xsensors.battery_mgmt = nullptr;
    xsensors.o3_no2 = nullptr;
    SensorManager xsensor_manager(xsensors);

    // Both PM sensors support temp/hum
    REQUIRE_CALL(mock_pm_a_local, supports_temp_hum()).RETURN(true);
    REQUIRE_CALL(mock_pm_b_local, supports_temp_hum()).RETURN(true);

    // PM sensor A provides temp/hum data (3 iterations for averaging)
    REQUIRE_CALL(mock_pm_a_local, temp_hum_data())
        .RETURN(TempHumData{20.0f, 50.0f});
    REQUIRE_CALL(mock_pm_a_local, temp_hum_data())
        .RETURN(TempHumData{22.0f, 52.0f});
    REQUIRE_CALL(mock_pm_a_local, temp_hum_data())
        .RETURN(TempHumData{24.0f, 54.0f});

    // PM sensor B provides temp/hum data (3 iterations for averaging)
    REQUIRE_CALL(mock_pm_b_local, temp_hum_data())
        .RETURN(TempHumData{15.0f, 60.0f});
    REQUIRE_CALL(mock_pm_b_local, temp_hum_data())
        .RETURN(TempHumData{17.0f, 62.0f});
    REQUIRE_CALL(mock_pm_b_local, temp_hum_data())
        .RETURN(TempHumData{19.0f, 64.0f});

    // PM sensors also provide PM data
    EXPECT_READ(mock_pm_a_local,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f,
                        2.5f, 5.0f, 10.0f}),
                true);
    EXPECT_READ(mock_pm_a_local,
                (PMData{2.0f, 3.5f, 11.0f, 2.1f, 3.6f, 11.1f, 1.3f, 1.5f, 2.0f,
                        3.5f, 6.0f, 11.0f}),
                true);
    EXPECT_READ(mock_pm_a_local,
                (PMData{3.0f, 4.5f, 12.0f, 3.1f, 4.6f, 12.1f, 2.3f, 2.5f, 3.0f,
                        4.5f, 7.0f, 12.0f}),
                true);

    EXPECT_READ(mock_pm_b_local,
                (PMData{10.0f, 20.0f, 100.0f, 10.1f, 20.1f, 100.1f, 10.3f,
                        10.5f, 10.0f, 20.0f, 50.0f, 100.0f}),
                true);
    EXPECT_READ(mock_pm_b_local,
                (PMData{11.0f, 21.0f, 101.0f, 11.1f, 21.1f, 101.1f, 11.3f,
                        11.5f, 11.0f, 21.0f, 51.0f, 101.0f}),
                true);
    EXPECT_READ(mock_pm_b_local,
                (PMData{12.0f, 22.0f, 102.0f, 12.1f, 22.1f, 102.1f, 12.3f,
                        12.5f, 12.0f, 22.0f, 52.0f, 102.0f}),
                true);

    auto result = xsensor_manager.start_measures(3);

    // Verify temp_hum_a comes from pms_a (avg of 20,22,24 = 22; avg of 50,52,54
    // = 52)
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(22.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(52.0f, 0.001f));

    // Verify temp_hum_b comes from pms_b (avg of 15,17,19 = 17; avg of 60,62,64
    // = 62)
    REQUIRE_THAT(result.temp_hum_b.temperature, WithinAbs(17.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_b.humidity, WithinAbs(62.0f, 0.001f));

    // Verify PM data is present
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(2.0f, 0.001f)); // avg of 1,2,3
    REQUIRE_THAT(result.pm_b.pm_01,
                 WithinAbs(11.0f, 0.001f)); // avg of 10,11,12
  }

  SECTION("Only pms_b supports temp/hum") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockPMSensor mock_pm_a_local;
    MockPMSensor mock_pm_b_local;
    Sensors xsensors;
    xsensors.temp_hum = nullptr; // No dedicated sensor
    xsensors.co2 = nullptr;
    xsensors.pms_a = &mock_pm_a_local;
    xsensors.pms_b = &mock_pm_b_local;
    xsensors.tvoc_nox = nullptr;
    xsensors.battery_mgmt = nullptr;
    xsensors.o3_no2 = nullptr;
    SensorManager xsensor_manager(xsensors);

    // Only PM sensor B supports temp/hum
    REQUIRE_CALL(mock_pm_a_local, supports_temp_hum()).RETURN(false);
    REQUIRE_CALL(mock_pm_b_local, supports_temp_hum()).RETURN(true);

    // PM sensor B provides temp/hum data
    REQUIRE_CALL(mock_pm_b_local, temp_hum_data())
        .RETURN(TempHumData{18.0f, 65.0f});

    // PM sensors provide PM data
    EXPECT_READ(mock_pm_a_local,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f,
                        2.5f, 5.0f, 10.0f}),
                true);
    EXPECT_READ(mock_pm_b_local,
                (PMData{10.0f, 20.0f, 100.0f, 10.1f, 20.1f, 100.1f, 10.3f,
                        10.5f, 10.0f, 20.0f, 50.0f, 100.0f}),
                true);

    auto result = xsensor_manager.start_measures(1);

    // Verify temp_hum_a is invalid (pms_a doesn't support temp/hum)
    REQUIRE(result.temp_hum_a.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_a.humidity == MeasuresInvalid::HUMIDITY);

    // Verify temp_hum_b comes from pms_b
    REQUIRE_THAT(result.temp_hum_b.temperature, WithinAbs(18.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_b.humidity, WithinAbs(65.0f, 0.001f));

    // Verify PM data is present
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(10.0f, 0.001f));
  }

  SECTION("Dedicated sensor ignores PM temp/hum") {
    // Allow RTOS calls (timing doesn't matter for this test)
    ALLOW_CALL(mock_rtos, get_time_ms_impl()).RETURN(0);
    ALLOW_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    MockTempHumSensor mock_tempHum_local;
    MockPMSensor mock_pm_a_local;
    MockPMSensor mock_pm_b_local;
    Sensors xsensors;
    xsensors.temp_hum = &mock_tempHum_local; // Dedicated sensor present
    xsensors.co2 = nullptr;
    xsensors.pms_a = &mock_pm_a_local;
    xsensors.pms_b = &mock_pm_b_local;
    xsensors.tvoc_nox = nullptr;
    xsensors.battery_mgmt = nullptr;
    xsensors.o3_no2 = nullptr;
    SensorManager xsensor_manager(xsensors);

    // Both PM sensors support temp/hum, but should be ignored
    ALLOW_CALL(mock_pm_a_local, supports_temp_hum()).RETURN(true);
    ALLOW_CALL(mock_pm_b_local, supports_temp_hum()).RETURN(true);

    // Dedicated sensor provides data
    EXPECT_READ(mock_tempHum_local, (TempHumData{25.0f, 55.0f}), true);

    // PM sensors provide PM data only (temp/hum methods should NOT be called)
    EXPECT_READ(mock_pm_a_local,
                (PMData{1.0f, 2.5f, 10.0f, 1.1f, 2.6f, 10.1f, 0.3f, 0.5f, 1.0f,
                        2.5f, 5.0f, 10.0f}),
                true);
    EXPECT_READ(mock_pm_b_local,
                (PMData{10.0f, 20.0f, 100.0f, 10.1f, 20.1f, 100.1f, 10.3f,
                        10.5f, 10.0f, 20.0f, 50.0f, 100.0f}),
                true);

    auto result = xsensor_manager.start_measures(1);

    // Verify temp_hum_a comes from dedicated sensor only
    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
    REQUIRE_THAT(result.temp_hum_a.humidity, WithinAbs(55.0f, 0.001f));

    // Verify temp_hum_b is invalid (PM sensors ignored when dedicated exists)
    REQUIRE(result.temp_hum_b.temperature == MeasuresInvalid::TEMPERATURE);
    REQUIRE(result.temp_hum_b.humidity == MeasuresInvalid::HUMIDITY);

    // Verify PM data is present
    REQUIRE_THAT(result.pm_a.pm_01, WithinAbs(1.0f, 0.001f));
    REQUIRE_THAT(result.pm_b.pm_01, WithinAbs(10.0f, 0.001f));
  }

  SECTION("Iteration delays to exactly 2 seconds when work takes 500ms") {
    // Simulate iteration taking 500ms
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(0);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(500);

    // Verify delay called with remaining 1500ms
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1500));

    // Set up sensor mocks (ALLOW_CALL first, then EXPECT_READ for proper order)
    ALLOW_CALL(mock_co2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_a, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_b, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_tvoc_nox, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_battery, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_o3no2, read(trompeloeil::_)).RETURN(false);
    EXPECT_READ(mock_tempHum, (TempHumData{25.0f, 50.0f}), true);

    Measures result = sensor_manager.start_measures(1);

    REQUIRE_THAT(result.temp_hum_a.temperature, WithinAbs(25.0f, 0.001f));
  }

  SECTION("Multiple iterations each delay correctly") {
    // Iteration 1: takes 300ms
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(0);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(300);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1700));

    // Iteration 2: takes 800ms
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(2000);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(2800);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1200));

    // Iteration 3: takes 100ms
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(4000);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(4100);
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1900));

    // Set up sensor mocks (ALLOW_CALL first, then EXPECT_READ for proper order)
    ALLOW_CALL(mock_co2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_a, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_b, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_tvoc_nox, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_battery, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_o3no2, read(trompeloeil::_)).RETURN(false);
    EXPECT_READ(mock_tempHum, (TempHumData{25.0f, 50.0f}), true).TIMES(3);

    sensor_manager.start_measures(3);
  }

  SECTION("No delay when iteration takes exactly 2 seconds") {
    // Iteration takes exactly 2000ms
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(0);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(2000);

    // No delay_ms should be called
    FORBID_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Set up sensor mocks (ALLOW_CALL first, then EXPECT_READ for proper order)
    ALLOW_CALL(mock_co2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_a, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_b, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_tvoc_nox, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_battery, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_o3no2, read(trompeloeil::_)).RETURN(false);
    EXPECT_READ(mock_tempHum, (TempHumData{25.0f, 50.0f}), true);

    sensor_manager.start_measures(1);
  }

  SECTION("No delay when iteration exceeds 2 seconds") {
    // Iteration takes 3000ms (exceeds 2 seconds)
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(0);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(3000);

    // No delay_ms should be called
    FORBID_CALL(mock_rtos, delay_ms_impl(trompeloeil::_));

    // Set up sensor mocks (ALLOW_CALL first, then EXPECT_READ for proper order)
    ALLOW_CALL(mock_co2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_a, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_b, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_tvoc_nox, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_battery, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_o3no2, read(trompeloeil::_)).RETURN(false);
    EXPECT_READ(mock_tempHum, (TempHumData{25.0f, 50.0f}), true);

    sensor_manager.start_measures(1);
  }

  SECTION("Very fast iteration delays almost full 2 seconds") {
    // Iteration takes only 1ms
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(0);
    REQUIRE_CALL(mock_rtos, get_time_ms_impl()).IN_SEQUENCE(seq).RETURN(1);

    // Verify delay called with remaining 1999ms
    REQUIRE_CALL(mock_rtos, delay_ms_impl(1999));

    // Set up sensor mocks (ALLOW_CALL first, then EXPECT_READ for proper order)
    ALLOW_CALL(mock_co2, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_a, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_pm_b, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_tvoc_nox, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_battery, read(trompeloeil::_)).RETURN(false);
    ALLOW_CALL(mock_o3no2, read(trompeloeil::_)).RETURN(false);
    EXPECT_READ(mock_tempHum, (TempHumData{25.0f, 50.0f}), true);

    sensor_manager.start_measures(1);
  }
}
