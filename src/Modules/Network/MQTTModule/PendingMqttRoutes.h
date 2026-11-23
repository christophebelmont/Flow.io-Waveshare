#pragma once

#include "Core/Services/IMqtt.h"
#include <freertos/FreeRTOS.h>

// Fixed mailbox: producers request publications; only the MQTT task drains.
// Zero means idle; other values encode priority + 1, retaining the highest.
class PendingMqttRoutes {
public:
    static constexpr uint8_t Capacity = 96;

    void mark(uint8_t route, MqttPublishPriority priority) {
        if (route >= Capacity) return;
        const uint8_t value = static_cast<uint8_t>(priority) + 1U;
        portENTER_CRITICAL(&mux_);
        if (value > requests_[route]) requests_[route] = value;
        portEXIT_CRITICAL(&mux_);
    }

    // Claim before calling transport; restore on deferral, merging any request
    // that arrived concurrently. Eligibility is inspected by the MQTT task.
    template<class Eligible, class Consume>
    bool drainOne(Eligible eligible, Consume consume) {
        for (uint8_t scanned = 0; scanned < Capacity; ++scanned) {
            const uint8_t route = cursor_;
            cursor_ = (cursor_ + 1U) % Capacity;
            if (!eligible(route)) continue;
            portENTER_CRITICAL(&mux_);
            const uint8_t value = requests_[route];
            requests_[route] = 0;
            portEXIT_CRITICAL(&mux_);
            if (!value) continue;
            const auto priority = static_cast<MqttPublishPriority>(value - 1U);
            const auto result = consume(route, priority);
            if (result == MqttAdmissionResult::Deferred) mark(route, priority);
            return result == MqttAdmissionResult::Accepted;
        }
        return false;
    }

private:
    uint8_t requests_[Capacity]{};
    uint8_t cursor_ = 0;
    portMUX_TYPE mux_ = portMUX_INITIALIZER_UNLOCKED;
};
