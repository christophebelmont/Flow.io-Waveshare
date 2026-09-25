#pragma once
#include "freertos/FreeRTOS.h"
#include <deque>
#include <vector>
#include <memory>
#include <cstring>
using BaseType_t = int;
using UBaseType_t = unsigned;
constexpr int pdFALSE = 0;
#define portENTER_CRITICAL_ISR(m) portENTER_CRITICAL(m)
#define portEXIT_CRITICAL_ISR(m) portEXIT_CRITICAL(m)
#define portYIELD_FROM_ISR() ((void)0)
struct FakeQueue {
    size_t capacity, itemSize;
    std::deque<std::vector<unsigned char>> items;
};
using QueueHandle_t = FakeQueue*;
inline std::vector<std::unique_ptr<FakeQueue>> fakeQueues;
inline QueueHandle_t xQueueCreate(size_t capacity, size_t itemSize) {
    fakeQueues.emplace_back(new FakeQueue{capacity, itemSize, {}});
    return fakeQueues.back().get();
}
inline BaseType_t xQueueSend(QueueHandle_t q, const void* item, TickType_t) {
    if (q->items.size() == q->capacity) return pdFALSE;
    const auto* bytes = static_cast<const unsigned char*>(item);
    q->items.emplace_back(bytes, bytes + q->itemSize);
    return pdTRUE;
}
inline BaseType_t xQueueSendFromISR(QueueHandle_t q, const void* item, BaseType_t*) {
    return xQueueSend(q, item, 0);
}
inline UBaseType_t uxQueueMessagesWaiting(QueueHandle_t q) { return q->items.size(); }
inline UBaseType_t uxQueueMessagesWaitingFromISR(QueueHandle_t q) { return q->items.size(); }
inline BaseType_t xQueueReceive(QueueHandle_t q, void* item, TickType_t) {
    if (q->items.empty()) return pdFALSE;
    std::memcpy(item, q->items.front().data(), q->itemSize);
    q->items.pop_front();
    return pdTRUE;
}
