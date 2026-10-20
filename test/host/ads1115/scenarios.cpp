#include "Modules/IOModule/IODrivers/Ads1115Driver.h"
#include <cassert>

int main()
{
    I2CBus bus;
    Ads1115DriverConfig cfg;
    cfg.pollMs = 20;
    adsFake.requestOk = false;
    Ads1115Driver driver("ads", &bus, cfg);
    assert(driver.begin());
    IOAnalogSample sample;
    driver.tick(20);
    assert(!driver.readSample(0, sample) && adsFake.channel == 0);
    adsFake.requestOk = true;
    driver.tick(40);
    adsFake.readOk = false;
    driver.tick(60);
    assert(!driver.readSample(0, sample));
    adsFake.readOk = true;
    driver.tick(80);
    driver.tick(100);
    assert(driver.readSample(0, sample) && sample.raw == 100 && sample.seq == 1);
    assert(adsFake.channel == 1);
    adsFake.statusOk = false;
    driver.tick(120);
    assert(!driver.readSample(1, sample));
    adsFake.statusOk = true;
    driver.tick(140);
    adsFake.ready = false;
    const unsigned requests = adsFake.requests;
    driver.tick(160);
    assert(adsFake.requests == requests);
    driver.tick(400);
    driver.tick(420);
    assert(adsFake.requests == requests + 1 && adsFake.channel == 1);
    adsFake.ready = true;
    driver.tick(440);
    assert(driver.readSample(1, sample) && sample.raw == 101);
    assert(!bus.locked);

    adsFake = {};
    cfg.differentialPairs = true;
    Ads1115Driver diff("diff", &bus, cfg);
    assert(diff.begin());
    diff.tick(20);
    assert(diff.readSample(0, sample) && sample.raw == 100);
    assert(!diff.readSample(1, sample));
    diff.tick(40);
    assert(diff.readSample(1, sample) && sample.raw == 101);
}
