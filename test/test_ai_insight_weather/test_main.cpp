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

void test_prompt_builder_combines_history_weather_and_strict_constraints()
{
    PoolHistorySnapshot history{};
    history.generatedAtUtc = 1788500615ULL;
    history.previousDay.valid = true;
    history.previousDay.complete = true;
    history.previousDay.localDate = 20260903U;
    history.previousDay.filtrationRuntimeValid = true;
    history.previousDay.filtrationRunningSec = 21600U;
    history.previousDay.filtrationRuntimeMinutes = 360U;
    history.previousDay.filtrationRuntimeHours = 6.0f;
    history.previousDay.filtrationObservedSec = 86400U;
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
    history.today.filtrationRuntimeValid = true;
    history.today.filtrationRunningSec = 7200U;
    history.today.filtrationRuntimeMinutes = 120U;
    history.today.filtrationRuntimeHours = 2.0f;
    history.today.filtrationObservedSec = 28800U;
    history.today.ph = {true, 4U, 7.25f, 7.20f, 7.18f, 7.25f, 7.21f};

    AiWeatherStatus weather{};
    weather.state = AiWeatherState::Ready;
    weather.weather.available = true;
    weather.weather.latitude = 43.604652;
    weather.weather.longitude = 1.444209;
    weather.weather.currentAirTemperatureC = {true, 27.5f};
    weather.weather.forecast24hPrecipitationMm = {true, 24U, 3.2f};

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
    TEST_ASSERT_NOT_NULL(strstr(prompt, "Ne demande jamais de monter ou baisser le pH, l'ORP"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "N'ajoute aucun titre, sous-titre, libellé"));
    TEST_ASSERT_NULL(strstr(prompt, "état actuel, dynamique et météo, point d'attention"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "2026-09-03"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "électrolyse au sel"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "filtration : 6.00 h (360 min) sur 24.00 h observées"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "dernière 7.20"));
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
    TEST_ASSERT_NOT_NULL(strstr(weatherText, "Aucune donnée météo"));
    TEST_ASSERT_NOT_NULL(strstr(prompt, "Historique piscine : indisponible"));
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
    RUN_TEST(test_weather_parser_separates_previous_and_forecast_windows);
    RUN_TEST(test_weather_parser_rejects_misaligned_hourly_arrays);
    RUN_TEST(test_prompt_builder_combines_history_weather_and_strict_constraints);
    RUN_TEST(test_prompt_builder_reports_missing_context_without_inventing_values);
    RUN_TEST(test_openai_responses_parser_extracts_all_output_text_parts);
    RUN_TEST(test_openai_responses_parser_reports_api_error);
    RUN_TEST(test_openai_responses_parser_keeps_long_rate_limit_diagnostic);
    RUN_TEST(test_openai_responses_parser_rejects_incomplete_output);
    RUN_TEST(test_pool_insight_reuse_window_is_strictly_one_hour);
    return UNITY_END();
}
