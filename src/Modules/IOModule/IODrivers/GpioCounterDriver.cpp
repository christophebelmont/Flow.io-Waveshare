/**
 * @file GpioCounterDriver.cpp
 * @brief Implementation file.
 */

#include "GpioCounterDriver.h"
#include <esp_heap_caps.h>

namespace {
portMUX_TYPE gGpioCounterMux = portMUX_INITIALIZER_UNLOCKED;

int gpioInterruptModeForCounter_(bool activeHigh, uint8_t edgeMode)
{
    (void)activeHigh;
    (void)edgeMode;
    // The ISR edge classifier relies on seeing both signal transitions so it can
    // keep lastLogicalState in sync. Restricting the hardware interrupt to a
    // single edge makes some subsequent pulses look like "same state" and they
    // get dropped.
    return CHANGE;
}
}

GpioCounterDriver::GpioCounterDriver(const char* driverId,
                                     uint8_t pin,
                                     bool activeHigh,
                                     uint8_t inputPullMode,
                                     uint8_t edgeMode,
                                     uint32_t counterDebounceUs)
    : driverId_(driverId),
      pin_(pin),
      activeHigh_(activeHigh),
      inputPullMode_(inputPullMode),
      edgeMode_(edgeMode),
      counterDebounceUs_(counterDebounceUs)
{
}

bool GpioCounterDriver::begin()
{
    if (!state_) {
        state_ = static_cast<RuntimeState*>(
            heap_caps_calloc(1, sizeof(RuntimeState), MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT)
        );
    }
    if (!state_) return false;

    if (inputPullMode_ == 1) pinMode(pin_, INPUT_PULLUP);
    else if (inputPullMode_ == 2) pinMode(pin_, INPUT_PULLDOWN);
    else pinMode(pin_, INPUT);

    int rawLevel = digitalRead(pin_);
    state_->lastLogicalState = activeHigh_ ? (rawLevel == HIGH) : (rawLevel == LOW);
    state_->pulseCount = 0;
    state_->lastPulseUs = 0;
    state_->irqCallCount = 0;
    state_->transitionCount = 0;
    state_->ignoredSameStateCount = 0;
    state_->ignoredWrongEdgeCount = 0;
    state_->ignoredDebounceCount = 0;
    attachInterruptArg(pin_, &GpioCounterDriver::handleInterruptThunk_, this, gpioInterruptModeForCounter_(activeHigh_, edgeMode_));
    return true;
}

bool GpioCounterDriver::read(bool& on) const
{
    int level = digitalRead(pin_);
    on = activeHigh_ ? (level == HIGH) : (level == LOW);
    return true;
}

bool GpioCounterDriver::readCount(int32_t& count) const
{
    portENTER_CRITICAL(&gGpioCounterMux);
    count = state_ ? state_->pulseCount : 0;
    portEXIT_CRITICAL(&gGpioCounterMux);
    return true;
}

bool GpioCounterDriver::readDebugStats(IODigitalCounterDebugStats& out) const
{
    portENTER_CRITICAL(&gGpioCounterMux);
    out.pin = pin_;
    out.edgeMode = edgeMode_;
    out.activeHigh = activeHigh_;
    out.logicalState = state_ ? state_->lastLogicalState : false;
    out.pulseCount = state_ ? state_->pulseCount : 0;
    out.irqCalls = state_ ? state_->irqCallCount : 0;
    out.transitions = state_ ? state_->transitionCount : 0;
    out.ignoredSameState = state_ ? state_->ignoredSameStateCount : 0;
    out.ignoredWrongEdge = state_ ? state_->ignoredWrongEdgeCount : 0;
    out.ignoredDebounce = state_ ? state_->ignoredDebounceCount : 0;
    out.lastPulseUs = state_ ? state_->lastPulseUs : 0;
    portEXIT_CRITICAL(&gGpioCounterMux);
    return true;
}

void IRAM_ATTR GpioCounterDriver::handleInterruptThunk_(void* arg)
{
    if (!arg) return;
    static_cast<GpioCounterDriver*>(arg)->handleInterrupt_();
}

void IRAM_ATTR GpioCounterDriver::handleInterrupt_()
{
    RuntimeState* state = state_;
    if (!state) return;
    state->irqCallCount = state->irqCallCount + 1U;
    int level = digitalRead(pin_);
    const bool logicalOn = activeHigh_ ? (level == HIGH) : (level == LOW);
    const uint32_t nowUs = micros();
    const bool wasLogicalOn = state->lastLogicalState;
    if (logicalOn == wasLogicalOn) {
        state->ignoredSameStateCount = state->ignoredSameStateCount + 1U;
        return;
    }
    state->lastLogicalState = logicalOn;
    state->transitionCount = state->transitionCount + 1U;

    const bool isRising = (!wasLogicalOn && logicalOn);
    const bool isFalling = (wasLogicalOn && !logicalOn);
    const bool shouldCount =
        ((edgeMode_ == 1U) && isRising) ||
        ((edgeMode_ == 0U) && isFalling) ||
        ((edgeMode_ == 2U) && (isRising || isFalling));
    if (!shouldCount) {
        state->ignoredWrongEdgeCount = state->ignoredWrongEdgeCount + 1U;
        return;
    }

    if (counterDebounceUs_ > 0 && state->lastPulseUs != 0U) {
        if ((uint32_t)(nowUs - state->lastPulseUs) < counterDebounceUs_) {
            state->ignoredDebounceCount = state->ignoredDebounceCount + 1U;
            return;
        }
    }

    portENTER_CRITICAL_ISR(&gGpioCounterMux);
    if (state->pulseCount < INT32_MAX) state->pulseCount = state->pulseCount + 1;
    state->lastPulseUs = nowUs;
    portEXIT_CRITICAL_ISR(&gGpioCounterMux);
}
