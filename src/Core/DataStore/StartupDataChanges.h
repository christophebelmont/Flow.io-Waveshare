#pragma once
#include "Core/DataKeys.h"
#include <freertos/FreeRTOS.h>

/** Coalesce state notifications until their initial publication is complete.
 * The dispatcher is the sole drain caller. Producers may mark concurrently.
 * No callbacks execute under the spinlock, including queue operations.
 */
class StartupDataChanges {
public:
    bool mark(DataKey key) {
        if (key > DataKeys::ReservedMax) return false;
        portENTER_CRITICAL(&mux_);
        const bool pending = !complete_;
        if (pending) bits_[key / 32U] |= uint32_t(1) << (key % 32U);
        portEXIT_CRITICAL(&mux_);
        return pending;
    }

    template<class Publish>
    void drain(uint16_t budget, Publish publish) {
        for (uint16_t sent = 0; sent < budget; ++sent) {
            DataKey key = 0;
            bool found = false;
            portENTER_CRITICAL(&mux_);
            if (!complete_) {
                for (uint16_t scanned = 0; scanned < KeyCount; ++scanned) {
                    key = cursor_;
                    cursor_ = (cursor_ + 1U) % KeyCount;
                    const uint32_t mask = uint32_t(1) << (key % 32U);
                    if (bits_[key / 32U] & mask) {
                        bits_[key / 32U] &= ~mask;
                        found = true;
                        break;
                    }
                }
                if (!found) complete_ = true;
            }
            portEXIT_CRITICAL(&mux_);
            if (!found) return;
            if (!publish(key)) {
                // A concurrent write may already have marked this key again.
                // OR preserves both that write and the refused notification.
                portENTER_CRITICAL(&mux_);
                bits_[key / 32U] |= uint32_t(1) << (key % 32U);
                portEXIT_CRITICAL(&mux_);
                return;
            }
        }
    }
private:
    static constexpr uint16_t KeyCount = DataKeys::ReservedMax + 1U;
    uint32_t bits_[(KeyCount + 31U) / 32U]{};
    uint16_t cursor_ = 0;
    bool complete_ = false;
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
