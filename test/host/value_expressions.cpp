#include "Modules/IOModule/ValueConfig.h"
#include "Modules/PoolHistoryModule/ValueHistory.h"
#include <cassert>
#include <cmath>
#include <cstring>
#include <memory>
#include <string>
#include <cstdio>
#include <thread>
#include <atomic>

using namespace ValueExpression;
static void near(double actual, double expected) {
    if (std::abs(actual - expected) > 1e-9) std::fprintf(stderr, "%.17g != %.17g\n", actual, expected);
    assert(std::isfinite(actual) && std::abs(actual - expected) <= 1e-9);
}
static Program compiled(const char* text) { Program p; assert(compile(text, p)); return p; }
static Number calculate(const char* text, const Number* inputs = nullptr) {
    const auto p = compiled(text);
    const double parameters[] = {2, 3, 4, 5};
    Number result;
    assert(evaluate(p, parameters, inputs, result)); return result;
}
static void parserTests() {
    near(calculate("2 + 3 * 4 - 8 / 2").current, 10);
    near(calculate("-(2 + 3) * -2").current, 10);
    near(calculate("max(abs(-3), min(8, 2)) + clamp(9, 0, 5)").current, 8);
    near(calculate("k0 + k1 * k2 - k3").current, 9);
    near(calculate("1.25e2 + .5 - 2E-1").current, 125.3);
    const auto p = compiled("a01+i01.count+i01.rate+i01.total+i01.flow+v00+a01");
    const ValueId expected[] = {1, 33, 49, 65, 81, 96};
    assert(p.dependencyCount == 6);
    assert(compiled("v04").dependencies[0] == ValueIds::Derived + 4);
    for (uint8_t i = 0; i < 6; ++i) assert(p.dependencies[i] == expected[i]);
    for (const char* bad : {"", " ", "nan", "NaN", "inf", "1e309", "0x10", "1e", ".", "1..2",
            "a1", "a32", "v05", "v15", "v16", "i16.count", "i01", "i01.totalx", "A01", "k4", "k00",
            "sin(1)", "min(1)", "min(1,2,3)", "abs()", "clamp(1,2)", "1 2", "2+", "(2", "2)",
            "1;2", "a01[0]", "+1"}) {
        Program invalid;
        assert(!compile(bad, invalid)); assert(invalid.size == 0);
        assert(!ValueConfig::validateExpression(bad));
    }
    Program invalid;
    assert(!compile(nullptr, invalid));
    assert(!compile(std::string(TextCapacity, ' ').c_str(), invalid));
    assert(!compile((std::string(33,'(')+"1"+std::string(33,')')).c_str(), invalid));
    assert(compile((std::string(31,'-')+"1").c_str(), invalid));
    assert(!compile((std::string(32,'-')+"1").c_str(), invalid));
    assert(compile("1+(2+(3+(4+(5+(6+(7+8))))))", invalid));
    assert(!compile("1+(2+(3+(4+(5+(6+(7+(8+9)))))))", invalid));
    const auto full = compiled("a00+a01+a02+a03+a04+a05+a06+a07+a08+a09+a10+a11+a12+a13+a14+a15");
    assert(full.dependencyCount == MaxDependencies && full.size == 31);
    assert(!compile("a00+a01+a02+a03+a04+a05+a06+a07+a08+a09+a10+a11+a12+a13+a14+a15+a16", invalid));
    assert(ValueConfig::validateExpression("i01.total*k0+k1"));
    double parameters[4]{}; Number result;
    for (const char* text : {"1/0", "0/0", "1e308*1e308", "clamp(1,5,0)"})
        assert(!evaluate(compiled(text), parameters, nullptr, result));
    auto malformed = compiled("1"); malformed.code[0].op = Op::Add; assert(!validate(malformed));
    malformed = compiled("a01"); malformed.code[0].operand = 16; assert(!validate(malformed));
    malformed = compiled("k0"); malformed.code[0].operand = 4; assert(!validate(malformed));
    // Finite differences, including changing both arguments and switching branches.
    Number inputs[] = {{10, 12, 2}, {2, 3, 1}};
    near(calculate("a01*a02", inputs).delta, 16);
    near(calculate("a01/a02", inputs).delta, -1);
    inputs[0] = {-2, 3, 5}; near(calculate("abs(a01)", inputs).delta, 1);
    inputs[0] = {1, 5, 4}; inputs[1] = {3, 4, 1};
    near(calculate("min(a01,a02)", inputs).delta, 3);
    near(calculate("max(a01,a02)", inputs).delta, 2);
    near(calculate("clamp(a01, 2, 4)", inputs).delta, 2);
}
static void topologyTests() {
    ValueConfig config; ConfigStore store; config.registerConfig(store, 1);
    assert(config.definitions[0].exprVar.validateText("a01+1"));
    assert(!config.definitions[0].exprVar.validateText("bad"));
    ValueRegistry registry; assert(registry.begin()); assert(registry.define(1, {}));
    config.definitions[0].enabled = config.definitions[1].enabled = true;
    std::strcpy(config.definitions[0].expr, "v01+1");
    std::strcpy(config.definitions[1].expr, "v00+1");
    assert(!config.resolve(registry)); assert(registry.used() == 1);
    std::strcpy(config.definitions[1].expr, "a02"); // Missing physical source.
    assert(!config.resolve(registry)); assert(registry.used() == 1);
    std::strcpy(config.definitions[1].expr, "v02"); // Disabled dependency.
    assert(!config.resolve(registry)); assert(registry.used() == 1);
    std::strcpy(config.definitions[1].expr, "a01*k0+k1");
    config.definitions[2].enabled = true; // Constant-only expressions initialize at boot.
    std::strcpy(config.definitions[2].expr, "42");
    assert(config.resolve(registry));
    ValueSnapshot sample; assert(registry.read(98, sample)); near(sample.value.d, 42);
    ValueNumber n; n.f = 5; assert(registry.write(1,n,100));
    assert(registry.read(96,sample)); near(sample.value.d,6);
    const auto allocations = fakeHeapAllocations;
    config.definitions[1].parameters[0] = 2;
    config.definitions[1].parameters[1] = 3;
    config.applyParameters(registry);
    assert(registry.read(96,sample)); near(sample.value.d,14); assert(sample.generation == 1);
    // Persisted authoring changes do not alter the executable until reboot.
    std::strcpy(config.definitions[1].expr, "invalid");
    config.definitions[1].enabled = false;
    n.f = 6; assert(registry.write(1,n,200));
    assert(registry.read(96,sample)); near(sample.value.d,16);
    assert(!registry.write(96,n,300));
    config.definitions[1].parameters[0] = NAN;
    config.applyParameters(registry); assert(registry.read(96,sample));
    assert(sample.quality == ValueQuality::Invalid);
    const auto generation = sample.generation;
    config.applyParameters(registry); assert(registry.read(96,sample)); assert(sample.generation == generation);
    config.definitions[1].parameters[0] = 1; config.applyParameters(registry);
    assert(registry.read(96,sample)); assert(sample.quality == ValueQuality::Valid); near(sample.value.d,10);
    std::unique_ptr<unsigned char[]> memory(new unsigned char[ValueHistory::bytes(2,1)]);
    ValueHistory history; history.initialize(memory.get(),2,1);
    history.clock(500,20000ULL*24*ValueHistory::HourMs);
    registry.setObserver([](void* ctx, ValueId id, const ValueMetadata& meta, const ValueSnapshot& value,
                            const ValueSnapshot* source) {
        static_cast<ValueHistory*>(ctx)->observe(id,meta,value,source);
    }, &history);
    config.applyParameters(registry,1000); history.tick(2000);
    ValueHistoryRecord record;
    assert(history.read(98,false,0,record)); near(record.average(),42); assert(record.durationMs == 1000);
    assert(registry.read(98,sample)); assert(sample.timestampMs == 1000);
    assert(fakeHeapAllocations == allocations);
}
static void parameterConcurrencyTests() {
    ValueConfig config;
    config.definitions[0].enabled = true;
    std::strcpy(config.definitions[0].expr, "k0");
    ValueRegistry registry; assert(registry.begin()); assert(config.resolve(registry));
    std::atomic<bool> done{false};
    std::thread writer([&] {
        for (unsigned i = 0; i < 10000; ++i)
            ConfigDoubleAccess::write(config.definitions[0].parameters[0], i % 2 ? 1.234567890123 : 9.876543210987);
        done = true;
    });
    while (!done) {
        config.applyParameters(registry);
        ValueSnapshot sample; assert(registry.read(96,sample));
        assert(sample.value.d == 1 || sample.value.d == 1.234567890123 || sample.value.d == 9.876543210987);
    }
    writer.join();
}
static void counterTests() {
    ValueRegistry registry; assert(registry.begin());
    for (ValueId id : {ValueId(33),ValueId(34)})
        assert(registry.define(id,{ValueType::UInt64,AggregationMode::Counter}));
    assert(registry.define(1,{}));
    const double parameters[] = {2, 1, 0, 0};
    assert(registry.defineAffine(65,33,0.5,0,AggregationMode::Counter));
    assert(registry.defineProgram(96,compiled("i01.count*k0+i02.count+k1"),parameters,AggregationMode::Counter));
    assert(registry.defineProgram(97,compiled("v00*3"),parameters,AggregationMode::Counter));
    assert(registry.defineProgram(98,compiled("i01.total"),parameters,AggregationMode::Counter));
    assert(registry.defineProgram(99,compiled("i01.count/i02.count"),parameters,AggregationMode::Gauge));
    std::unique_ptr<unsigned char[]> memory(new unsigned char[ValueHistory::bytes(2,1)]);
    ValueHistory history; history.initialize(memory.get(),2,1); history.clock(0,20000ULL*24*ValueHistory::HourMs);
    registry.setObserver([](void* ctx, ValueId id, const ValueMetadata& meta, const ValueSnapshot& sample, const ValueSnapshot* source) {
        static_cast<ValueHistory*>(ctx)->observe(id,meta,sample,source);
    }, &history);
    const auto allocations = fakeHeapAllocations;
    ValueNumber n; n.u64 = (1ULL<<55); assert(registry.write(33,n,100)); assert(registry.write(34,n,100));
    ValueSnapshot snapshot; assert(registry.read(96,snapshot)); assert(snapshot.quality == ValueQuality::Valid);
    ++n.u64; assert(registry.write(33,n,200)); assert(registry.write(34,n,300));
    ValueHistoryRecord record;
    assert(history.read(96,false,0,record)); near(record.delta,3);
    const auto initialDiscontinuities = record.discontinuities;
    assert(history.read(97,false,0,record)); near(record.delta,9);
    assert(history.read(98,false,0,record)); near(record.delta,0.5);
    // An unchanged observation and an unrelated producer must not reuse an old delta.
    assert(registry.write(34,n,400));
    ValueNumber analog; analog.f=5; assert(registry.write(1,analog,400));
    assert(history.read(97,false,0,record)); near(record.delta,9);
    assert(registry.read(96,snapshot)); const auto before = snapshot.generation;
    n.u64=0; assert(registry.write(34,n,500,ValueQuality::Valid,1));
    assert(registry.read(96,snapshot)); assert(snapshot.generation == before+1 && !snapshot.deltaValid);
    assert(registry.read(99,snapshot)); assert(snapshot.quality == ValueQuality::Invalid);
    n.u64=1; assert(registry.write(34,n,600,ValueQuality::Valid,1));
    assert(history.read(96,false,0,record)); near(record.delta,4); assert(record.discontinuities == initialDiscontinuities+1);
    assert(registry.read(99,snapshot)); assert(snapshot.quality == ValueQuality::Valid);
    const double newParameters[] = {3,1,0,0}; assert(registry.setParameters(96,newParameters));
    assert(history.read(96,false,0,record)); near(record.delta,4); assert(record.discontinuities == initialDiscontinuities+2);
    n.u64=(1ULL<<55)+2; assert(registry.write(33,n,700));
    assert(history.read(96,false,0,record)); near(record.delta,7);
    assert(history.read(97,false,0,record)); near(record.delta,21);
    // UInt64 subtraction remains safe up to its maximum value.
    n.u64=UINT64_MAX-1; assert(registry.write(33,n,800,ValueQuality::Valid,2));
    ++n.u64; assert(registry.write(33,n,900,ValueQuality::Valid,2));
    assert(registry.read(96,snapshot)); assert(snapshot.deltaValid); near(snapshot.delta,3);
    assert(fakeHeapAllocations == allocations);
    std::printf("expression registry storage: %zu bytes\n", ValueRegistry::storageBytes());
}
int main() { parserTests(); topologyTests(); parameterConcurrencyTests(); counterTests(); std::puts("expressions: compiler, bounds, topology, validation, live parameters, UInt64 precision and history OK"); }
