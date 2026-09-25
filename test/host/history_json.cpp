#include "Modules/Network/WebInterfaceModule/HistoryJson.h"
#include <cassert>
#include <string>
#include <iostream>
int main() {
    DynamicJsonDocument doc(4096);
    PoolHistoryDaySummary day{};
    day.valid = true; day.localDate = 20260925;
    day.filtration.valid = true; day.filtration.runningSec = 0;
    day.waterTemperature.valid = true; day.waterTemperature.average = 0;
    day.waterTemperature.minimum = -1; day.waterTemperature.maximum = 1;
    HistoryJson::day(doc.to<JsonObject>(), day);
    assert(!doc.overflowed());
    assert(doc["filtration"]["seconds"].as<double>() == 0);
    assert(!doc["filtration"]["seconds"].isNull());
    assert(doc["heating"]["seconds"].isNull());
    assert(doc["metrics"][0]["average"].as<double>() == 0);
    assert(doc["metrics"][1]["average"].isNull());
    assert(doc["refill_litres"].isNull());
    assert(doc["metrics"].size() == 9);
    std::string body;
    serializeJson(doc, body);
    assert(body.size() * 8 + 8192 < 24 * 1024);
    ValueHistoryRecord record{};
    record.valid = true; record.period = 1000; record.rawDelta = UINT64_MAX;
    record.discontinuities = 1; record.boundaryUncertain = true;
    DynamicJsonDocument value(768);
    HistoryJson::record(value.to<JsonObject>(), record, false);
    assert(!value.overflowed());
    assert(value["start_utc"].as<uint64_t>() == 3600000);
    assert(std::string(value["raw_delta"].as<const char*>()) == "18446744073709551615");
    assert(value["average"].isNull());
    assert(value["discontinuities"].as<unsigned>() == 1);
    assert(value["boundary_uncertain"].as<bool>());
    body.clear(); serializeJson(value, body);
    assert(body.size() * 25 + 128 < 12 * 1024);
    std::cout << "History JSON: bounded documents, null vs zero, exact uint64, UTC periods and discontinuities OK\n";
}
