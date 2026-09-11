#include <ArduinoJson.h>
#include <unity.h>
#include <math.h>
#include <stdio.h>
#include <string.h>

#include "Modules/AiInsightModule/OpenMeteoWeatherParser.h"
#include "Modules/AiInsightModule/OpenAiResponsesParser.h"
#include "Modules/AiInsightModule/PoolInsightPromptBuilder.h"

void setUp() {}
void tearDown() {}

void test_weather_parser_exposes_seven_past_days_and_two_forecast_days()
{
    static constexpr char kPayload[] = R"json({
      "current": {
        "time": "2026-09-11T14:00",
        "temperature_2m": 13.5,
        "cloud_cover": 45.0,
        "wind_speed_10m": 18.0
      },
      "daily": {
        "time": ["2026-09-04","2026-09-05","2026-09-06","2026-09-07","2026-09-08","2026-09-09","2026-09-10","2026-09-11","2026-09-12"],
        "temperature_2m_min": [10,11,12,13,14,15,16,17,18],
        "temperature_2m_max": [20,21,22,23,24,25,26,27,28],
        "temperature_2m_mean": [15,16,17,18,19,20,21,22,23],
        "precipitation_sum": [0,1,2,3,4,5,6,7,8],
        "cloud_cover_mean": [10,20,30,40,50,60,70,80,90],
        "wind_speed_10m_max": [11,12,13,14,15,16,17,18,19],
        "shortwave_radiation_sum": [20,19,18,17,16,15,14,13,12]
      }
    })json";

    StaticJsonDocument<4096> document;
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
    TEST_ASSERT_EQUAL_UINT64(5000U, weather.observedAtUtc);
    TEST_ASSERT_EQUAL_UINT64(5000U, weather.fetchedAtUtc);
    TEST_ASSERT_TRUE(fabs(weather.latitude - 43.604652) < 0.000001);
    TEST_ASSERT_TRUE(fabs(weather.longitude - 1.444209) < 0.000001);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 13.5f, weather.currentAirTemperatureC.value);
    TEST_ASSERT_EQUAL_UINT32(20260911U, weather.currentLocalDate);
    TEST_ASSERT_EQUAL_UINT8(9U, weather.dailyCount);
    TEST_ASSERT_FALSE(weather.daily[6].forecast);
    TEST_ASSERT_TRUE(weather.daily[7].forecast);
    TEST_ASSERT_EQUAL_UINT32(20260910U, weather.daily[6].localDate);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 21.0f, weather.daily[6].meanAirTemperatureC.value);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 6.0f, weather.daily[6].precipitationMm.value);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 14.0f, weather.daily[6].shortwaveRadiationMjM2.value);
}

void test_weather_parser_rejects_misaligned_daily_arrays()
{
    static constexpr char kPayload[] = R"json({
      "current": {
        "time": "2026-09-11T14:00",
        "temperature_2m": 13.5,
        "cloud_cover": 45.0,
        "wind_speed_10m": 18.0
      },
      "daily": {
        "time": ["2026-09-10", "2026-09-11"],
        "temperature_2m_min": [10.0],
        "temperature_2m_max": [20.0, 21.0],
        "temperature_2m_mean": [15.0, 16.0],
        "precipitation_sum": [0.0, 0.0],
        "cloud_cover_mean": [20.0, 60.0],
        "wind_speed_10m_max": [8.0, 20.0],
        "shortwave_radiation_sum": [10.0, 11.0]
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
    TEST_ASSERT_EQUAL_STRING("weather daily arrays are invalid", error);
}

void test_prompt_builder_combines_history_weather_and_strict_constraints()
{
    TEST_ASSERT_LESS_THAN_UINT32((uint32_t)AiPoolInsightPreview::InstructionsCapacity,
                                 (uint32_t)strlen(PoolInsightPromptBuilder::instructions()));
    PoolHistorySnapshot history{};
    history.generatedAtUtc = 1788500615ULL;
    history.previousDay.valid = true;
    history.previousDay.complete = true;
    history.previousDay.localDate = 20260903U;
    history.previousDay.filtration.valid = true;
    history.previousDay.filtration.runningSec = 21600U;
    history.previousDay.filtration.runningMinutes = 360U;
    history.previousDay.filtration.runningHours = 6.0f;
    history.previousDay.filtration.observedSec = 86400U;
    history.previousDay.filtration.periods[1] = {true, 7200U, 21600U};
    history.previousDay.heating.valid = true;
    history.previousDay.heating.runningSec = 3600U;
    history.previousDay.heating.runningMinutes = 60U;
    history.previousDay.heating.runningHours = 1.0f;
    history.previousDay.heating.observedSec = 86400U;
    history.previousDay.heating.periods[3] = {true, 3600U, 21600U};
    history.previousDay.ph = {true, 10U, 7.30f, 7.25f, 7.20f, 7.35f, 7.27f};
    history.previousDay.orp = {true, 10U, 690.0f, 700.0f, 680.0f, 710.0f, 696.0f};
    history.completeDays[0] = history.previousDay;
    history.last7Days.availableDayCount = 1U;
    history.last7Days.averageDailyFiltrationValid = true;
    history.last7Days.totalFiltrationHours = 6.0f;
    history.last7Days.totalFiltrationMinutes = 360U;
    history.last7Days.averageDailyFiltrationHours = 6.0f;
    history.pool.available = true;
    history.pool.volumeValid = true;
    history.pool.volumeM3 = 50.0f;
    history.pool.disinfectionMethod = PoolDisinfectionMethod::SaltElectrolysis;
    history.today.valid = true;
    history.today.localDate = 20260904U;
    history.today.filtration.valid = true;
    history.today.filtration.runningSec = 7200U;
    history.today.filtration.runningMinutes = 120U;
    history.today.filtration.runningHours = 2.0f;
    history.today.filtration.observedSec = 28800U;
    history.today.ph = {true, 4U, 7.25f, 7.20f, 7.18f, 7.25f, 7.21f};

    AiWeatherStatus weather{};
    weather.state = AiWeatherState::Ready;
    weather.weather.available = true;
    weather.weather.latitude = 43.604652;
    weather.weather.longitude = 1.444209;
    weather.weather.currentAirTemperatureC = {true, 27.5f};
    weather.weather.currentLocalDate = 20260904U;
    weather.weather.dailyCount = 2U;
    weather.weather.daily[0].valid = true;
    weather.weather.daily[0].localDate = 20260903U;
    weather.weather.daily[0].meanAirTemperatureC = {true, 24.0f};
    weather.weather.daily[0].minimumAirTemperatureC = {true, 18.0f};
    weather.weather.daily[0].maximumAirTemperatureC = {true, 30.0f};
    weather.weather.daily[0].precipitationMm = {true, 3.2f};
    weather.weather.daily[1].valid = true;
    weather.weather.daily[1].forecast = true;
    weather.weather.daily[1].localDate = 20260904U;

    char weatherText[AiPoolInsightPreview::WeatherTextCapacity]{};
    char prompt[AiPoolInsightPreview::PromptCapacity]{};
    TEST_ASSERT_TRUE(PoolInsightPromptBuilder::build(&history,
                                                      weather,
                                                      weatherText,
                                                      sizeof(weatherText),
                                                      prompt,
                                                      sizeof(prompt)));
    TEST_ASSERT_NOT_NULL(strstr(weatherText, "43.604652, 1.444209"));
    TEST_ASSERT_NOT_NULL(strstr(weatherText, "27.5 °C"));
    TEST_ASSERT_NOT_NULL(strstr(PoolInsightPromptBuilder::instructions(),
                                "Ne demande jamais d'augmenter ou diminuer le pH"));
    TEST_ASSERT_NOT_NULL(strstr(PoolInsightPromptBuilder::instructions(),
                                "Produis uniquement 3 à 4 paragraphes courts"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "2026-09-03"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "électrolyse au sel"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "filtration total : 6.00 h (360 min) sur 24.00 h observées"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "matin 06-12 120/360 min"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "météo : air min 18.0, max 30.0, moyenne 24.0 °C"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "fin 7.20"));
}

void test_prompt_builder_reports_missing_context_without_inventing_values()
{
    AiWeatherStatus weather{};
    weather.state = AiWeatherState::Failed;
    snprintf(weather.message, sizeof(weather.message), "%s", "network unavailable");
    char weatherText[AiPoolInsightPreview::WeatherTextCapacity]{};
    char prompt[AiPoolInsightPreview::PromptCapacity]{};
    TEST_ASSERT_TRUE(PoolInsightPromptBuilder::build(nullptr,
                                                      weather,
                                                      weatherText,
                                                      sizeof(weatherText),
                                                      prompt,
                                                      sizeof(prompt)));
    TEST_ASSERT_NOT_NULL(strstr(weatherText, "données météo : indisponibles"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "Historique piscine : indisponible"));
}

void test_prompt_builder_accepts_fully_populated_seven_day_context()
{
    PoolHistorySnapshot history{};
    history.generatedAtUtc = 1789142400ULL;
    history.pool.available = true;
    history.pool.volumeValid = true;
    history.pool.volumeM3 = 52.0f;
    history.currentOperatingConfiguration.available = true;
    history.currentOperatingConfiguration.phSetpointValid = true;
    history.currentOperatingConfiguration.phSetpoint = 7.2f;
    history.currentOperatingConfiguration.orpSetpointValid = true;
    history.currentOperatingConfiguration.orpSetpointMv = 700.0f;
    history.currentOperatingConfiguration.heaterSetpointValid = true;
    history.currentOperatingConfiguration.heaterSetpointC = 28.0f;
    const PoolHistoryMetricSummary metric{true, 288U, 24.0f, 25.0f,
                                          23.0f, 26.0f, 24.5f};
    PoolHistoryActivitySummary activity{};
    activity.valid = true;
    activity.runningSec = 21600U;
    activity.runningMinutes = 360U;
    activity.runningHours = 6.0f;
    activity.observedSec = 86400U;
    for (uint8_t period = 0U; period < POOL_HISTORY_DAY_PERIOD_COUNT; ++period) {
        activity.periods[period] = {true, 5400U, 21600U};
    }
    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
        PoolHistoryDaySummary& day = history.completeDays[i];
        day.valid = true;
        day.complete = true;
        day.localDate = 20260910U - i;
        day.filtration = activity;
        day.heating = activity;
        day.ph = day.phSetpoint = day.orp = day.orpSetpoint = metric;
        day.waterTemperature = day.daytimeWaterTemperature = metric;
        day.nighttimeWaterTemperature = day.heaterSetpoint = day.airTemperature = metric;
        day.refillVolumeValid = true;
        day.refillEventsValid = true;
    }
    history.previousDay = history.completeDays[0];
    history.today = history.completeDays[0];
    history.today.complete = false;
    history.today.localDate = 20260911U;

    AiWeatherStatus weather{};
    weather.state = AiWeatherState::Ready;
    weather.weather.available = true;
    weather.weather.latitude = 43.604652;
    weather.weather.longitude = 1.444209;
    weather.weather.currentLocalDate = 20260911U;
    weather.weather.currentAirTemperatureC = {true, 27.5f};
    weather.weather.currentCloudCoverPercent = {true, 40.0f};
    weather.weather.currentWindSpeedKmh = {true, 15.0f};
    weather.weather.dailyCount = POOL_WEATHER_DAILY_CAPACITY;
    for (uint8_t i = 0U; i < POOL_WEATHER_DAILY_CAPACITY; ++i) {
        PoolWeatherDaySummary& day = weather.weather.daily[i];
        day.valid = true;
        day.localDate = 20260904U + i;
        day.forecast = day.localDate >= weather.weather.currentLocalDate;
        day.minimumAirTemperatureC = {true, 16.0f};
        day.maximumAirTemperatureC = {true, 30.0f};
        day.meanAirTemperatureC = {true, 23.0f};
        day.precipitationMm = {true, 2.0f};
        day.meanCloudCoverPercent = {true, 35.0f};
        day.maximumWindSpeedKmh = {true, 22.0f};
        day.shortwaveRadiationMjM2 = {true, 18.0f};
    }

    char weatherText[AiPoolInsightPreview::WeatherTextCapacity]{};
    char prompt[AiPoolInsightPreview::PromptCapacity]{};
    TEST_ASSERT_TRUE(PoolInsightPromptBuilder::build(&history, weather,
                                                      weatherText, sizeof(weatherText),
                                                      prompt, sizeof(prompt)));
    TEST_ASSERT_LESS_THAN_UINT32((uint32_t)sizeof(prompt), (uint32_t)strlen(prompt));
}

void test_openai_responses_parser_extracts_all_output_text_parts()
{
    static constexpr char kPayload[] = R"json({
      "id": "resp_pool_123",
      "model": "gpt-test-2026-09-08",
      "status": "completed",
      "output": [
        {"type":"reasoning","summary":[]},
        {"type":"message","role":"assistant","content":[
          {"type":"output_text","text":"Le bassin est stable."},
          {"type":"refusal","refusal":""},
          {"type":"output_text","text":"La hausse de température explique la filtration plus longue."}
        ]}
      ]
    })json";

    StaticJsonDocument<2048> document;
    TEST_ASSERT_FALSE(deserializeJson(document, kPayload));
    OpenAiResponsesParser::Result result{};
    char text[512]{};
    char error[128]{};
    TEST_ASSERT_TRUE(OpenAiResponsesParser::parse(document.as<JsonVariantConst>(),
                                                   result,
                                                   text,
                                                   sizeof(text),
                                                   error,
                                                   sizeof(error)));
    TEST_ASSERT_EQUAL_STRING("resp_pool_123", result.responseId);
    TEST_ASSERT_EQUAL_STRING("gpt-test-2026-09-08", result.model);
    TEST_ASSERT_EQUAL_STRING(
        "Le bassin est stable.\nLa hausse de température explique la filtration plus longue.",
        text);
}

void test_openai_responses_parser_reports_api_error()
{
    static constexpr char kPayload[] = R"json({
      "error": {"message":"The configured model is unavailable.","type":"invalid_request_error","code":"model_not_found"}
    })json";

    StaticJsonDocument<512> document;
    TEST_ASSERT_FALSE(deserializeJson(document, kPayload));
    OpenAiResponsesParser::Result result{};
    char text[128]{};
    char error[128]{};
    TEST_ASSERT_FALSE(OpenAiResponsesParser::parse(document.as<JsonVariantConst>(),
                                                    result,
                                                    text,
                                                    sizeof(text),
                                                    error,
                                                    sizeof(error)));
    TEST_ASSERT_EQUAL_STRING(
        "OpenAI [model_not_found]: The configured model is unavailable.",
        error);
    TEST_ASSERT_EQUAL_STRING("", text);
}

void test_openai_responses_parser_keeps_long_rate_limit_diagnostic()
{
    static constexpr char kPayload[] = R"json({
      "error": {
        "message":"This deliberately long quota diagnostic exceeds the small destination buffer while preserving a useful machine-readable reason for the operator.",
        "type":"insufficient_quota",
        "code":"insufficient_quota"
      }
    })json";

    StaticJsonDocument<512> document;
    TEST_ASSERT_FALSE(deserializeJson(document, kPayload));
    char error[64]{};
    TEST_ASSERT_TRUE(OpenAiResponsesParser::extractApiError(
        document.as<JsonVariantConst>(), error, sizeof(error)));
    TEST_ASSERT_EQUAL_UINT8('\0', error[sizeof(error) - 1U]);
    TEST_ASSERT_NOT_NULL(strstr(error, "insufficient_quota"));
}

void test_openai_responses_parser_rejects_incomplete_output()
{
    static constexpr char kPayload[] = R"json({
      "id":"resp_incomplete",
      "model":"gpt-test",
      "status":"incomplete",
      "incomplete_details":{"reason":"max_output_tokens"},
      "output":[{"type":"message","content":[
        {"type":"output_text","text":"Texte tronqué"}
      ]}]
    })json";

    StaticJsonDocument<768> document;
    TEST_ASSERT_FALSE(deserializeJson(document, kPayload));
    OpenAiResponsesParser::Result result{};
    char text[128]{};
    char error[128]{};
    TEST_ASSERT_FALSE(OpenAiResponsesParser::parse(document.as<JsonVariantConst>(),
                                                    result,
                                                    text,
                                                    sizeof(text),
                                                    error,
                                                    sizeof(error)));
    TEST_ASSERT_EQUAL_STRING("OpenAI response is incomplete (max_output_tokens)", error);
    TEST_ASSERT_EQUAL_STRING("", text);
}

void test_pool_insight_reuse_window_is_strictly_one_hour()
{
    constexpr uint64_t generatedAt = 100000U;
    constexpr uint32_t lifetimeSec = 3600U;
    TEST_ASSERT_TRUE(aiPoolInsightIsReusable(AiPoolInsightState::Ready,
                                              generatedAt,
                                              generatedAt + 3599U,
                                              true,
                                              lifetimeSec));
    TEST_ASSERT_FALSE(aiPoolInsightIsReusable(AiPoolInsightState::Ready,
                                               generatedAt,
                                               generatedAt + 3600U,
                                               true,
                                               lifetimeSec));
    TEST_ASSERT_FALSE(aiPoolInsightIsReusable(AiPoolInsightState::Failed,
                                               generatedAt,
                                               generatedAt + 1U,
                                               true,
                                               lifetimeSec));
    TEST_ASSERT_FALSE(aiPoolInsightIsReusable(AiPoolInsightState::Ready,
                                               generatedAt,
                                               generatedAt - 1U,
                                               true,
                                               lifetimeSec));
    TEST_ASSERT_FALSE(aiPoolInsightIsReusable(AiPoolInsightState::Ready,
                                               generatedAt,
                                               generatedAt + 1U,
                                               false,
                                               lifetimeSec));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_weather_parser_exposes_seven_past_days_and_two_forecast_days);
    RUN_TEST(test_weather_parser_rejects_misaligned_daily_arrays);
    RUN_TEST(test_prompt_builder_combines_history_weather_and_strict_constraints);
    RUN_TEST(test_prompt_builder_reports_missing_context_without_inventing_values);
    RUN_TEST(test_prompt_builder_accepts_fully_populated_seven_day_context);
    RUN_TEST(test_openai_responses_parser_extracts_all_output_text_parts);
    RUN_TEST(test_openai_responses_parser_reports_api_error);
    RUN_TEST(test_openai_responses_parser_keeps_long_rate_limit_diagnostic);
    RUN_TEST(test_openai_responses_parser_rejects_incomplete_output);
    RUN_TEST(test_pool_insight_reuse_window_is_strictly_one_hour);
    return UNITY_END();
}
