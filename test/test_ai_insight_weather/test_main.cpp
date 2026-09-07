#include <ArduinoJson.h>
#include <unity.h>
#include <math.h>

#include "Modules/AiInsightModule/OpenMeteoWeatherParser.h"

void setUp() {}
void tearDown() {}

void test_weather_parser_separates_previous_and_forecast_windows()
{
    static constexpr char kPayload[] = R"json({
      "current": {
        "time": 2000,
        "temperature_2m": 13.5,
        "cloud_cover": 45.0,
        "wind_speed_10m": 18.0
      },
      "hourly": {
        "time": [1000, 2000, 3000, 4000],
        "temperature_2m": [10.0, 12.0, 14.0, 16.0],
        "precipitation": [0.1, 0.2, 1.0, 2.0],
        "cloud_cover": [20.0, 30.0, 60.0, 80.0],
        "wind_speed_10m": [8.0, 10.0, 20.0, 30.0],
        "shortwave_radiation": [0.0, 10.0, 100.0, 200.0]
      }
    })json";

    StaticJsonDocument<2048> document;
    TEST_ASSERT_FALSE(deserializeJson(document, kPayload));

    PoolWeatherSnapshot weather{};
    char error[96]{};
    TEST_ASSERT_TRUE(OpenMeteoWeatherParser::parse(document.as<JsonVariantConst>(),
                                                    43.604652,
                                                    1.444209,
                                                    5000U,
                                                    weather,
                                                    error,
                                                    sizeof(error)));
    TEST_ASSERT_TRUE(weather.available);
    TEST_ASSERT_EQUAL_UINT64(2000U, weather.observedAtUtc);
    TEST_ASSERT_EQUAL_UINT64(5000U, weather.fetchedAtUtc);
    TEST_ASSERT_TRUE(fabs(weather.latitude - 43.604652) < 0.000001);
    TEST_ASSERT_TRUE(fabs(weather.longitude - 1.444209) < 0.000001);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.5f, weather.currentAirTemperatureC.value);
    TEST_ASSERT_EQUAL_UINT16(2U, weather.previous24hAirTemperatureC.sampleCount);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 10.0f, weather.previous24hAirTemperatureC.minimum);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 12.0f, weather.previous24hAirTemperatureC.maximum);
    TEST_ASSERT_EQUAL_UINT16(2U, weather.forecast24hAirTemperatureC.sampleCount);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 14.0f, weather.forecast24hAirTemperatureC.minimum);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 16.0f, weather.forecast24hAirTemperatureC.maximum);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 0.3f, weather.previous24hPrecipitationMm.value);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 3.0f, weather.forecast24hPrecipitationMm.value);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 70.0f, weather.forecast24hCloudCoverPercent.value);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 30.0f, weather.forecast24hMaximumWindSpeedKmh.value);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 150.0f, weather.forecast24hShortwaveRadiationWm2.value);
}

void test_weather_parser_rejects_misaligned_hourly_arrays()
{
    static constexpr char kPayload[] = R"json({
      "current": {
        "time": 2000,
        "temperature_2m": 13.5,
        "cloud_cover": 45.0,
        "wind_speed_10m": 18.0
      },
      "hourly": {
        "time": [1000, 3000],
        "temperature_2m": [10.0],
        "precipitation": [0.0, 0.0],
        "cloud_cover": [20.0, 60.0],
        "wind_speed_10m": [8.0, 20.0],
        "shortwave_radiation": [0.0, 100.0]
      }
    })json";

    StaticJsonDocument<1536> document;
    TEST_ASSERT_FALSE(deserializeJson(document, kPayload));

    PoolWeatherSnapshot weather{};
    char error[96]{};
    TEST_ASSERT_FALSE(OpenMeteoWeatherParser::parse(document.as<JsonVariantConst>(),
                                                     43.604652,
                                                     1.444209,
                                                     5000U,
                                                     weather,
                                                     error,
                                                     sizeof(error)));
    TEST_ASSERT_FALSE(weather.available);
    TEST_ASSERT_EQUAL_STRING("weather hourly arrays are invalid", error);
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_weather_parser_separates_previous_and_forecast_windows);
    RUN_TEST(test_weather_parser_rejects_misaligned_hourly_arrays);
    return UNITY_END();
}
