/**
 * @file Ads1115Driver.cpp
 * @brief Implementation file.
 */

#include "Ads1115Driver.h"

Ads1115Driver::Ads1115Driver(const char* driverId, I2CBus* bus, const Ads1115DriverConfig& cfg)
    : driverId_(driverId), bus_(bus), cfg_(cfg), ads_(cfg.address, bus ? bus->wire() : &Wire)
{
}

bool Ads1115Driver::begin()
{
    if (!bus_) return false;
    if (!bus_->lock(50)) return false;

    bool ok = ads_.begin() && ads_.isConnected();
    if (ok) {
        ads_.setGain(cfg_.gain);
        ads_.setDataRate(cfg_.dataRate);
        ready_ = true;
        requestNext_(millis());
    }

    bus_->unlock();
    return ok;
}

void Ads1115Driver::tick(uint32_t nowMs)
{
    if (!ready_ || !bus_) return;
    if ((uint32_t)(nowMs - lastTickMs_) < cfg_.pollMs) return;
    lastTickMs_ = nowMs;

    if (!bus_->lock(20)) return;

    if (!requested_) {
        requestNext_(nowMs);
        bus_->unlock();
        return;
    }

    (void)ads_.getError();
    const bool conversionReady = ads_.isReady();
    const bool statusOk = ads_.getError() == ADS1X15_OK;
    // The slowest ADS1115 conversion is 125 ms (8 SPS). Allow twice that
    // duration, then restart the same channel if the conversion never ends.
    if (!statusOk || (!conversionReady && uint32_t(nowMs - requestedAtMs_) >= 250U)) {
        requested_ = false;
    } else if (conversionReady) {
        int16_t raw = ads_.getValue();
        if (ads_.getError() != ADS1X15_OK) {
            requested_ = false;
            bus_->unlock();
            return;
        }
        float v = toVolts_(raw);

        if (cfg_.differentialPairs) {
            if (nextDiffPair_ == 1) {
                validDiff23_ = true;
                rawDiff23_ = raw;
                vDiff23_ = v;
                ++seqDiff23_;
            } else {
                validDiff01_ = true;
                rawDiff01_ = raw;
                vDiff01_ = v;
                ++seqDiff01_;
            }
            nextDiffPair_ ^= 1U;
        } else {
            validSingle_[nextSingleCh_] = true;
            rawSingle_[nextSingleCh_] = raw;
            vSingle_[nextSingleCh_] = v;
            ++seqSingle_[nextSingleCh_];
            nextSingleCh_ = (uint8_t)((nextSingleCh_ + 1) % 4);
        }

        requestNext_(nowMs);
    }

    bus_->unlock();
}

bool Ads1115Driver::readVoltsChannel(uint8_t ch, float& outV) const
{
    if (ch > 3 || !validSingle_[ch]) return false;
    outV = vSingle_[ch];
    return true;
}

bool Ads1115Driver::readRawChannel(uint8_t ch, int16_t& outRaw) const
{
    if (ch > 3 || !validSingle_[ch]) return false;
    outRaw = rawSingle_[ch];
    return true;
}

bool Ads1115Driver::readVoltsDifferential01(float& outV) const
{
    if (!validDiff01_) return false;
    outV = vDiff01_;
    return true;
}

bool Ads1115Driver::readRawDifferential01(int16_t& outRaw) const
{
    if (!validDiff01_) return false;
    outRaw = rawDiff01_;
    return true;
}

bool Ads1115Driver::readVoltsDifferential23(float& outV) const
{
    if (!validDiff23_) return false;
    outV = vDiff23_;
    return true;
}

bool Ads1115Driver::readRawDifferential23(int16_t& outRaw) const
{
    if (!validDiff23_) return false;
    outRaw = rawDiff23_;
    return true;
}

bool Ads1115Driver::readSampleSeqChannel(uint8_t ch, uint32_t& outSeq) const
{
    if (ch > 3 || !validSingle_[ch]) return false;
    outSeq = seqSingle_[ch];
    return true;
}

bool Ads1115Driver::readSampleSeqDifferential01(uint32_t& outSeq) const
{
    if (!validDiff01_) return false;
    outSeq = seqDiff01_;
    return true;
}

bool Ads1115Driver::readSampleSeqDifferential23(uint32_t& outSeq) const
{
    if (!validDiff23_) return false;
    outSeq = seqDiff23_;
    return true;
}

bool Ads1115Driver::readSample(uint8_t channel, IOAnalogSample& out) const
{
    out = IOAnalogSample{};
    if (!cfg_.differentialPairs) {
        int16_t raw = 0;
        float volts = 0.0f;
        uint32_t seq = 0;
        if (!readRawChannel(channel, raw)) return false;
        if (!readVoltsChannel(channel, volts)) return false;
        if (!readSampleSeqChannel(channel, seq)) return false;
        out.value = volts;
        out.raw = raw;
        out.seq = seq;
        out.hasRaw = true;
        out.hasSeq = true;
        return true;
    }

    // Differential mode exposes pair index: 0 -> 0-1, 1 -> 2-3.
    int16_t raw = 0;
    float volts = 0.0f;
    uint32_t seq = 0;
    if (channel == 0) {
        if (!readRawDifferential01(raw)) return false;
        if (!readVoltsDifferential01(volts)) return false;
        if (!readSampleSeqDifferential01(seq)) return false;
    } else {
        if (!readRawDifferential23(raw)) return false;
        if (!readVoltsDifferential23(volts)) return false;
        if (!readSampleSeqDifferential23(seq)) return false;
    }

    out.value = volts;
    out.raw = raw;
    out.seq = seq;
    out.hasRaw = true;
    out.hasSeq = true;
    return true;
}

void Ads1115Driver::requestNext_(uint32_t nowMs)
{
    (void)ads_.getError();
    if (cfg_.differentialPairs) {
        if (nextDiffPair_ == 0) {
            ads_.requestADC_Differential_0_1();
        } else {
            ads_.requestADC_Differential_2_3();
        }
    } else {
        ads_.requestADC(nextSingleCh_);
    }

    requested_ = ads_.getError() == ADS1X15_OK;
    if (requested_) requestedAtMs_ = nowMs;
}

float Ads1115Driver::toVolts_(int16_t raw)
{
    float v = ads_.toVoltage(raw);
    if (v <= ADS1X15_INVALID_VOLTAGE) {
        return (float)raw * cfg_.voltLsb;
    }
    return v;
}
