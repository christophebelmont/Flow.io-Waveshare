#include "Core/Values/Value.h"
#include <cassert>
#include <cmath>

int main() {
    assert(validDisplayRange(-10.0, 45.0));
    assert(!validDisplayRange(4.0, 4.0));
    assert(!validDisplayRange(5.0, 4.0));
    assert(!validDisplayRange(NAN, 4.0));
    assert(!validDisplayRange(1.0, INFINITY));

    const auto orp = defaultValueDisplayRange(0);
    const auto ph = defaultValueDisplayRange(1);
    const auto waterTemperature = defaultValueDisplayRange(4);
    const auto airTemperature = defaultValueDisplayRange(5);
    const auto noDefault = defaultValueDisplayRange(6);
    assert(orp.hasRange && orp.min == 350.0 && orp.max == 900.0);
    assert(ph.hasRange && ph.min == 6.4 && ph.max == 8.4);
    assert(waterTemperature.hasRange && waterTemperature.min == 0.0 && waterTemperature.max == 40.0);
    assert(airTemperature.hasRange && airTemperature.min == -10.0 && airTemperature.max == 45.0);
    assert(!noDefault.hasRange);
}
