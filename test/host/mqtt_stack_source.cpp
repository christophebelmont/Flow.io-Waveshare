#include <atomic>
#include <cassert>
#include <chrono>
#include <future>
#include <mutex>
#include "Core/TaskHealth.h"

using SemaphoreHandle_t = std::mutex*;
std::promise<void>* stopAttempt = nullptr;
bool clientTaskAlive = true;
unsigned xSemaphoreTake(SemaphoreHandle_t mutex, unsigned wait) {
    if (wait == 0U) return mutex->try_lock();
    stopAttempt->set_value();
    mutex->lock();
    return pdTRUE;
}
void xSemaphoreGive(SemaphoreHandle_t mutex) { mutex->unlock(); }
int esp_mqtt_client_stop(void*) {
    clientTaskAlive = false;
    return 0;
}

class MQTTModule {
public:
    void* client_ = this;
    SemaphoreHandle_t clientStackMutex_ = nullptr;
    std::atomic<TaskHandle_t> clientStackTask_{nullptr};
    uint32_t clientStackGeneration_ = 1;
    bool suppressDisconnectEvent_ = false, clientStarted_ = true;
    static TaskStackReadStatus acquireClientTaskStack_(void*, TaskHandle_t&, uint32_t&);
    static void releaseClientTaskStack_(void*);
    void stopClient_(bool);
};

// PRODUCTION_METHODS

int main() {
    std::mutex mutex;
    int task = 0;
    MQTTModule mqtt;
    mqtt.clientStackMutex_ = &mutex;
    mqtt.clientStackTask_.store(&task);
    TaskHandle_t handle = nullptr;
    uint32_t generation = 0;
    assert(mqtt.acquireClientTaskStack_(&mqtt, handle, generation) == TaskStackReadStatus::Acquired);
    assert(handle == &task && generation == 1);
    std::promise<void> attempt;
    stopAttempt = &attempt;
    auto stopping = std::async(std::launch::async, [&] { mqtt.stopClient_(true); });
    attempt.get_future().wait();
    assert(stopping.wait_for(std::chrono::milliseconds(0)) == std::future_status::timeout);
    assert(clientTaskAlive); // The reserved handle remains alive throughout the scan.
    mqtt.releaseClientTaskStack_(&mqtt);
    stopping.get();
    assert(!clientTaskAlive && mqtt.suppressDisconnectEvent_);
    assert(mqtt.acquireClientTaskStack_(&mqtt, handle, generation) == TaskStackReadStatus::Unavailable);
    assert(!handle && generation == 2);
    // A recreated task may reuse the exact same handle.
    mqtt.clientStackTask_.store(&task);
    assert(mqtt.acquireClientTaskStack_(&mqtt, handle, generation) == TaskStackReadStatus::Acquired);
    assert(handle == &task && generation == 2);
    mqtt.releaseClientTaskStack_(&mqtt);
    // Contention must not block sysmon.
    std::promise<void> held, release;
    auto holder = std::async(std::launch::async, [&] {
        std::lock_guard<std::mutex> lock(mutex);
        held.set_value();
        release.get_future().wait();
    });
    held.get_future().wait();
    assert(mqtt.acquireClientTaskStack_(&mqtt, handle, generation) == TaskStackReadStatus::Busy);
    release.set_value();
    holder.get();
}
