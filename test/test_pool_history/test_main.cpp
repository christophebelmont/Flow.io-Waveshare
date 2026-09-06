#include <unity.h>

#include "Modules/PoolHistoryModule/PoolHistoryAccumulator.h"
#include "Modules/PoolHistoryModule/PoolHistoryPersistence.h"

void setUp() {}
void tearDown() {}

void test_daily_metrics_and_filtration_are_aggregated()
{
    PoolHistoryAccumulator history{};
    TEST_ASSERT_EQUAL_UINT8((uint8_t)PoolHistoryDayTransition::Initialized,
                            (uint8_t)history.alignDay(20260904U, 20260903U, 1788472800ULL));

    history.addSample(PoolHistoryMetric::Ph, 7.20f, 1788500000ULL);
    history.addSample(PoolHistoryMetric::Ph, 7.40f, 1788500300ULL);
    history.addSample(PoolHistoryMetric::Ph, 7.10f, 1788500600ULL);
    history.observeFiltration(10000U, true, 1788500610ULL);
    history.observeFiltration(5000U, false, 1788500615ULL);

    PoolHistorySnapshot snapshot{};
    history.snapshot(1788500615ULL, snapshot);
    TEST_ASSERT_TRUE(snapshot.today.valid);
    TEST_ASSERT_FALSE(snapshot.today.complete);
    TEST_ASSERT_EQUAL_UINT32(20260904U, snapshot.today.localDate);
    TEST_ASSERT_EQUAL_UINT32(3U, snapshot.today.ph.sampleCount);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.20f, snapshot.today.ph.first);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.10f, snapshot.today.ph.last);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.10f, snapshot.today.ph.minimum);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.40f, snapshot.today.ph.maximum);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 7.233333f, snapshot.today.ph.average);
    TEST_ASSERT_EQUAL_UINT32(10U, snapshot.today.filtrationRunningSec);
    TEST_ASSERT_EQUAL_UINT32(15U, snapshot.today.filtrationObservedSec);
}

void test_day_rollover_closes_previous_day_and_starts_empty_today()
{
    PoolHistoryAccumulator history{};
    history.alignDay(20260904U, 20260903U, 1788472800ULL);
    history.addSample(PoolHistoryMetric::Orp, 680.0f, 1788559100ULL);
    history.observeFiltration(60000U, true, 1788559160ULL);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)PoolHistoryDayTransition::AdvancedOneDay,
                            (uint8_t)history.alignDay(20260905U,
                                                      20260904U,
                                                      1788559200ULL));

    PoolHistorySnapshot snapshot{};
    history.snapshot(1788559210ULL, snapshot);
    TEST_ASSERT_TRUE(snapshot.previousDay.valid);
    TEST_ASSERT_TRUE(snapshot.previousDay.complete);
    TEST_ASSERT_EQUAL_UINT32(20260904U, snapshot.previousDay.localDate);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 680.0f, snapshot.previousDay.orp.last);
    TEST_ASSERT_TRUE(snapshot.today.valid);
    TEST_ASSERT_FALSE(snapshot.today.complete);
    TEST_ASSERT_EQUAL_UINT32(20260905U, snapshot.today.localDate);
    TEST_ASSERT_FALSE(snapshot.today.orp.valid);
}

void test_non_consecutive_date_change_discards_obsolete_previous_day()
{
    PoolHistoryAccumulator history{};
    history.alignDay(20260901U, 20260831U, 1788213600ULL);
    history.addSample(PoolHistoryMetric::WaterTemperature, 25.0f, 1788220000ULL);

    TEST_ASSERT_EQUAL_UINT8((uint8_t)PoolHistoryDayTransition::Realigned,
                            (uint8_t)history.alignDay(20260904U,
                                                      20260903U,
                                                      1788472800ULL));

    PoolHistorySnapshot snapshot{};
    history.snapshot(1788473000ULL, snapshot);
    TEST_ASSERT_FALSE(snapshot.previousDay.valid);
    TEST_ASSERT_EQUAL_UINT32(20260904U, snapshot.today.localDate);
}

void test_restore_keeps_only_current_and_immediate_previous_dates()
{
    PoolHistoryDayState current{};
    current.valid = true;
    current.complete = true;
    current.localDate = 20260904U;
    current.dayStartUtc = 1788472800ULL;
    current.observedUntilUtc = 1788500000ULL;
    current.metrics[(uint8_t)PoolHistoryMetric::AirTemperature].sampleCount = 1U;
    current.metrics[(uint8_t)PoolHistoryMetric::AirTemperature].first = 29.0f;
    current.metrics[(uint8_t)PoolHistoryMetric::AirTemperature].last = 29.0f;
    current.metrics[(uint8_t)PoolHistoryMetric::AirTemperature].minimum = 29.0f;
    current.metrics[(uint8_t)PoolHistoryMetric::AirTemperature].maximum = 29.0f;
    current.metrics[(uint8_t)PoolHistoryMetric::AirTemperature].sum = 29.0;

    PoolHistoryDayState previous{};
    previous.valid = true;
    previous.localDate = 20260903U;
    previous.dayStartUtc = 1788386400ULL;
    previous.observedUntilUtc = 1788472700ULL;

    PoolHistoryAccumulator history{};
    history.restoreForDate(20260904U,
                           20260903U,
                           1788472800ULL,
                           &previous,
                           &current);

    PoolHistorySnapshot snapshot{};
    history.snapshot(1788500100ULL, snapshot);
    TEST_ASSERT_FALSE(snapshot.today.complete);
    TEST_ASSERT_TRUE(snapshot.previousDay.complete);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 29.0f, snapshot.today.airTemperature.last);
}

void test_persistence_round_trip_and_checksum_validation()
{
    PoolHistoryAccumulator history{};
    history.alignDay(20260904U, 20260903U, 1788472800ULL);
    history.addSample(PoolHistoryMetric::WaterTemperature, 24.5f, 1788500000ULL);
    history.observeFiltration(12345U, true, 1788500012ULL);

    uint8_t encoded[PoolHistoryPersistence::EncodedSize]{};
    size_t encodedLength = 0U;
    TEST_ASSERT_TRUE(PoolHistoryPersistence::encode(history.todayState(),
                                                     encoded,
                                                     sizeof(encoded),
                                                     encodedLength));
    TEST_ASSERT_EQUAL_UINT32((uint32_t)PoolHistoryPersistence::EncodedSize,
                             (uint32_t)encodedLength);

    PoolHistoryDayState decoded{};
    TEST_ASSERT_TRUE(PoolHistoryPersistence::decode(encoded, encodedLength, decoded));
    TEST_ASSERT_EQUAL_UINT32(20260904U, decoded.localDate);
    TEST_ASSERT_EQUAL_UINT64(12345ULL, decoded.filtrationRunningMs);
    TEST_ASSERT_EQUAL_UINT32(1U,
        decoded.metrics[(uint8_t)PoolHistoryMetric::WaterTemperature].sampleCount);

    encoded[20] ^= 0x01U;
    TEST_ASSERT_FALSE(PoolHistoryPersistence::decode(encoded, encodedLength, decoded));
}

int main()
{
    UNITY_BEGIN();
    RUN_TEST(test_daily_metrics_and_filtration_are_aggregated);
    RUN_TEST(test_day_rollover_closes_previous_day_and_starts_empty_today);
    RUN_TEST(test_non_consecutive_date_change_discards_obsolete_previous_day);
    RUN_TEST(test_restore_keeps_only_current_and_immediate_previous_dates);
    RUN_TEST(test_persistence_round_trip_and_checksum_validation);
    return UNITY_END();
}
