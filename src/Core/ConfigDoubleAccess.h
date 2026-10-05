#pragma once
#include <stddef.h>
#include <freertos/FreeRTOS.h>
#include <freertos/semphr.h>

/** Coherent access to live 64-bit config scalars on 32-bit targets.
 * Only hold this lock while copying values; never during NVS IO or callbacks. */
namespace ConfigDoubleAccess {
inline StaticSemaphore_t mutexStorage{};
inline SemaphoreHandle_t mutex = xSemaphoreCreateMutexStatic(&mutexStorage);
class Lock {
public:
    Lock() { xSemaphoreTake(mutex, portMAX_DELAY); }
    ~Lock() { xSemaphoreGive(mutex); }
    Lock(const Lock&) = delete;
    Lock& operator=(const Lock&) = delete;
};
inline void copy(const double* source, double* destination, size_t count) {
    Lock lock;
    for (size_t i = 0; i < count; ++i) destination[i] = source[i];
}
inline double read(const double& source) {
    double value;
    copy(&source, &value, 1);
    return value;
}
inline bool replace(double& destination, double value, double& previous) {
    Lock lock;
    previous = destination;
    if (destination == value) return false;
    destination = value;
    return true;
}
inline void write(double& destination, double value) {
    double previous;
    (void)replace(destination, value, previous);
}
}
