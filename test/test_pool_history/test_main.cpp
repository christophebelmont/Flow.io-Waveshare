#include <unity.h>

#include "Modules/PoolHistoryModule/PoolHistoryAccumulator.h"
#include "Modules/PoolHistoryModule/PoolHistoryPersistence.h"

namespace {

constexpr uint32_t kDatesForSep04[POOL_HISTORY_COMPLETE_DAY_COUNT] = {
    20260903U, 20260902U, 20260901U, 20260831U, 20260830U, 20260829U, 20260828U
};
constexpr uint64_t kStartsForSep04[POOL_HISTORY_COMPLETE_DAY_COUNT] = {
    1788386400ULL, 1788300000ULL, 1788213600ULL, 1788127200ULL,
    1788040800ULL, 1787954400ULL, 1787868000ULL
};
constexpr uint32_t kDatesForSep05[POOL_HISTORY_COMPLETE_DAY_COUNT] = {
    20260904U, 20260903U, 20260902U, 20260901U, 20260831U, 20260830U, 20260829U
};
constexpr uint64_t kStartsForSep05[POOL_HISTORY_COMPLETE_DAY_COUNT] = {
    1788472800ULL, 1788386400ULL, 1788300000ULL, 1788213600ULL,
    1788127200ULL, 1788040800ULL, 1787954400ULL
};

PoolHistorySnapshot snapshotForSep04(const PoolHistoryAccumulator& history)
{
    PoolHistorySnapshot snapshot{};
    PoolCharacteristics pool{};
    history.snapshot(1788500615ULL, kDatesForSep04, kStartsForSep04,
                     8U, 20U, pool, snapshot);
    return snapshot;
}

}  // namespace

void setUp() {}
void tearDown() {}

void test_daily_metrics_filtration_temperature_and_refill_are_aggregated()
{
    PoolHistoryAccumulator history{};
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PoolHistoryDayTransition::Initialized,
                            (uint8_t)history.alignDay(20260904U, 1788472800ULL,
                                                      kDatesForSep04));
    history.addSample(PoolHistoryMetric::Ph, 7.20f, 1788500000ULL);
    history.addSample(PoolHistoryMetric::Ph, 7.40f, 1788500300ULL);
    history.addSample(PoolHistoryMetric::Ph, 7.10f, 1788500600ULL);
    history.addSample(PoolHistoryMetric::WaterTemperature, 26.0f, 1788500600ULL);
    history.addWaterTemperatureSample(26.0f, true, 1788500600ULL);
    history.addWaterTemperatureSample(24.0f, false, 1788500600ULL);
    history.observeFiltration(3600000U, true, 1788500610ULL);
    history.observeFiltration(60000U, false, 1788500615ULL);
    history.observeRefill(1800000U, true, 2.0f, true, 1788500615ULL);

    const PoolHistorySnapshot snapshot = snapshotForSep04(history);
    TEST_ASSERT_TRUE(snapshot.today.valid);
    TEST_ASSERT_EQUAL_UINT32(3U, snapshot.today.ph.sampleCount);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.10f, snapshot.today.ph.minimum);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.40f, snapshot.today.ph.maximum);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.233333f, snapshot.today.ph.average);
    TEST_ASSERT_EQUAL_UINT32(60U, snapshot.today.filtrationRuntimeMinutes);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, snapshot.today.filtrationRuntimeHours);
    TEST_ASSERT_TRUE(snapshot.today.dayToNightTemperatureVariationValid);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, -2.0f,
                             snapshot.today.dayToNightTemperatureVariationC);
    TEST_ASSERT_TRUE(snapshot.today.refillVolumeValid);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, snapshot.today.refillVolumeLitres);
    TEST_ASSERT_EQUAL_UINT32(1U, snapshot.today.refillEventCount);
}

void test_sampling_gate_requires_ten_continuous_minutes()
{
    PoolHistorySamplingGate gate{};
    gate.initialize(true, true);
    gate.accrue(599999U, true);
    TEST_ASSERT_FALSE(gate.eligible(600000ULL));
    gate.accrue(1U, true);
    TEST_ASSERT_TRUE(gate.eligible(600000ULL));
    TEST_ASSERT_EQUAL_UINT32(0U, gate.maximumEligibleSampleAgeMs(600000ULL, 900000U));
    gate.accrue(300000U, true);
    TEST_ASSERT_EQUAL_UINT32(300000U,
                             gate.maximumEligibleSampleAgeMs(600000ULL, 900000U));
    gate.update(true, false);
    TEST_ASSERT_FALSE(gate.eligible(600000ULL));
    gate.update(true, true);
    gate.accrue(600000U, true);
    TEST_ASSERT_TRUE(gate.eligible(600000ULL));
    gate.accrue(1U, false);
    TEST_ASSERT_FALSE(gate.eligible(600000ULL));
}

void test_day_rollover_exposes_only_complete_days()
{
    PoolHistoryAccumulator history{};
    history.alignDay(20260904U, 1788472800ULL, kDatesForSep04);
    history.addSample(PoolHistoryMetric::Orp, 680.0f, 1788559100ULL);
    history.observeFiltration(7200000U, true, 1788559160ULL);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PoolHistoryDayTransition::AdvancedOneDay,
                            (uint8_t)history.alignDay(20260905U, 1788559200ULL,
                                                      kDatesForSep05));

    PoolHistorySnapshot snapshot{};
    PoolCharacteristics pool{};
    history.snapshot(1788559210ULL, kDatesForSep05, kStartsForSep05,
                     8U, 20U, pool, snapshot);
    TEST_ASSERT_TRUE(snapshot.completeDays[0].valid);
    TEST_ASSERT_TRUE(snapshot.completeDays[0].complete);
    TEST_ASSERT_EQUAL_UINT32(20260904U, snapshot.completeDays[0].localDate);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 680.0f, snapshot.completeDays[0].orp.last);
    TEST_ASSERT_EQUAL_UINT8(1U, snapshot.last7Days.availableDayCount);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 2.0f, snapshot.last7Days.totalFiltrationHours);
    TEST_ASSERT_FALSE(snapshot.today.orp.valid);
}

void test_restore_places_records_in_expected_calendar_slots_and_marks_missing_days()
{
    PoolHistoryDayState records[3]{};
    records[0].valid = true;
    records[0].localDate = 20260904U;
    records[0].dayStartUtc = 1788472800ULL;
    PoolHistoryMetricState& air = records[0].metrics[(uint8_t)PoolHistoryMetric::AirTemperature];
    air.sampleCount = 1U;
    air.first = air.last = air.minimum = air.maximum = 29.0f;
    air.sum = 29.0;
    records[1].valid = true;
    records[1].localDate = 20260903U;
    records[1].dayStartUtc = 1788386400ULL;
    records[2].valid = true;
    records[2].localDate = 20260901U;
    records[2].dayStartUtc = 1788213600ULL;

    PoolHistoryAccumulator history{};
    history.restoreForDate(20260904U, 1788472800ULL, kDatesForSep04, records, 3U);
    const PoolHistorySnapshot snapshot = snapshotForSep04(history);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 29.0f, snapshot.today.airTemperature.last);
    TEST_ASSERT_TRUE(snapshot.completeDays[0].valid);
    TEST_ASSERT_FALSE(snapshot.completeDays[1].valid);
    TEST_ASSERT_EQUAL_UINT32(20260902U, snapshot.completeDays[1].localDate);
    TEST_ASSERT_TRUE(snapshot.completeDays[2].valid);
    TEST_ASSERT_EQUAL_UINT8(2U, snapshot.last7Days.availableDayCount);
}

void test_unknown_refill_flow_invalidates_volume_but_keeps_events()
{
    PoolHistoryAccumulator history{};
    history.alignDay(20260904U, 1788472800ULL, kDatesForSep04);
    history.observeRefill(60000U, true, 0.0f, true, 1788500000ULL);
    const PoolHistorySnapshot snapshot = snapshotForSep04(history);
    TEST_ASSERT_FALSE(snapshot.today.refillVolumeValid);
    TEST_ASSERT_EQUAL_UINT32(1U, snapshot.today.refillEventCount);
}

void test_seven_complete_days_and_pool_characteristics_are_exposed()
{
    PoolHistoryDayState records[POOL_HISTORY_COMPLETE_DAY_COUNT]{};
    for (uint8_t i = 0U; i < POOL_HISTORY_COMPLETE_DAY_COUNT; ++i) {
        records[i].valid = true;
        records[i].complete = true;
        records[i].localDate = kDatesForSep04[i];
        records[i].dayStartUtc = kStartsForSep04[i];
        records[i].filtrationRunningMs = (uint64_t)(i + 1U) * 3600000ULL;
        records[i].filtrationObservedMs = 24ULL * 3600000ULL;
        records[i].refillVolumeValid = true;
        records[i].refillStateObserved = true;
        records[i].refillVolumeLitres = (double)(i + 1U);
        records[i].refillEventCount = 1U;
    }
    PoolHistoryAccumulator history{};
    history.restoreForDate(20260904U, 1788472800ULL, kDatesForSep04,
                           records, POOL_HISTORY_COMPLETE_DAY_COUNT);
    PoolCharacteristics pool{};
    pool.available = true;
    pool.volumeValid = true;
    pool.volumeM3 = 42.5f;
    pool.indoor = true;
    pool.automaticCoverPresent = true;
    pool.coverClosedAtNight = true;
    pool.disinfectionMethod = PoolDisinfectionMethod::ActiveOxygen;
    PoolHistorySnapshot snapshot{};
    history.snapshot(1788500615ULL, kDatesForSep04, kStartsForSep04,
                     7U, 19U, pool, snapshot);
    TEST_ASSERT_EQUAL_UINT8(7U, snapshot.last7Days.availableDayCount);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 28.0f, snapshot.last7Days.totalFiltrationHours);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 4.0f,
                             snapshot.last7Days.averageDailyFiltrationHours);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 28.0f,
                             snapshot.last7Days.totalRefillVolumeLitres);
    TEST_ASSERT_EQUAL_UINT32(7U, snapshot.last7Days.totalRefillEventCount);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 42.5f, snapshot.pool.volumeM3);
    TEST_ASSERT_TRUE(snapshot.pool.indoor);
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PoolDisinfectionMethod::ActiveOxygen,
                            (uint8_t)snapshot.pool.disinfectionMethod);
    TEST_ASSERT_EQUAL_UINT8(7U, snapshot.daytimeStartHour);
    TEST_ASSERT_EQUAL_UINT8(19U, snapshot.daytimeEndHour);
}

void test_persistence_v2_round_trip_and_checksum_validation()
{
    PoolHistoryAccumulator history{};
    history.alignDay(20260904U, 1788472800ULL, kDatesForSep04);
    history.addSample(PoolHistoryMetric::WaterTemperature, 24.5f, 1788500000ULL);
    history.addWaterTemperatureSample(24.5f, true, 1788500000ULL);
    history.observeFiltration(12345U, true, 1788500012ULL);
    history.observeRefill(3600000U, true, 1.5f, true, 1788500012ULL);

    uint8_t encoded[PoolHistoryPersistence::EncodedSize]{};
    size_t encodedLength = 0U;
    TEST_ASSERT_TRUE(PoolHistoryPersistence::encode(history.todayState(), encoded,
                                                     sizeof(encoded), encodedLength));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PoolHistoryPersistence::EncodedSize,
                             (uint32_t)encodedLength);
    PoolHistoryDayState decoded{};
    TEST_ASSERT_TRUE(PoolHistoryPersistence::decode(encoded, encodedLength, decoded));
    TEST_ASSERT_EQUAL_UINT64(12345ULL, decoded.filtrationRunningMs);
    TEST_ASSERT_EQUAL_UINT32(1U, decoded.daytimeWaterTemperature.sampleCount);
    TEST_ASSERT_TRUE(decoded.refillVolumeValid);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.5f, (float)decoded.refillVolumeLitres);
    TEST_ASSERT_EQUAL_UINT32(1U, decoded.refillEventCount);
    encoded[20] ^= 0x01U;
    TEST_ASSERT_FALSE(PoolHistoryPersistence::decode(encoded, encodedLength, decoded));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_daily_metrics_filtration_temperature_and_refill_are_aggregated);
    RUN_TEST(test_sampling_gate_requires_ten_continuous_minutes);
    RUN_TEST(test_day_rollover_exposes_only_complete_days);
    RUN_TEST(test_restore_places_records_in_expected_calendar_slots_and_marks_missing_days);
    RUN_TEST(test_unknown_refill_flow_invalidates_volume_but_keeps_events);
    RUN_TEST(test_seven_complete_days_and_pool_characteristics_are_exposed);
    RUN_TEST(test_persistence_v2_round_trip_and_checksum_validation);
    return UNITY_END();
}
