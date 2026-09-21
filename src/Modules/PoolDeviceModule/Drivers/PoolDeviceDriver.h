#pragma once
#include <variant>
#include "Core/Services/PoolActuatorTypes.h"

/** All implementations are non-blocking and owned by the pool-device task. */
class IPoolDeviceDriver {
public:
    virtual ~IPoolDeviceDriver() = default;
    virtual bool begin(const PoolDriverConfig&, const IOServiceV2*, const ModbusMasterService*, uint8_t owner) = 0;
    virtual void applyTarget(const PoolDeviceTarget&, uint32_t revision) = 0;
    virtual void tick(uint32_t nowMs, bool writesEnabled) = 0;
    virtual const PoolDeviceFeedback& readState() const = 0;
};

class PoolDriverBase : public IPoolDeviceDriver {
public:
    bool begin(const PoolDriverConfig&, const IOServiceV2*, const ModbusMasterService*, uint8_t) override;
    void applyTarget(const PoolDeviceTarget&, uint32_t revision) override;
    const PoolDeviceFeedback& readState() const override { return state_; }
protected:
    void applied_(const PoolDeviceTarget&, uint32_t revision, uint32_t now);
    void failed_(uint16_t error, uint32_t now);
    bool writeDigital_(IoId, bool, uint32_t);
    bool writable_(bool enabled);
    bool outputsAvailable_(uint32_t now);
    PoolDriverConfig config_{};
    const IOServiceV2* io_ = nullptr;
    const ModbusMasterService* bus_ = nullptr;
    uint8_t owner_ = 0;
    PoolDeviceTarget target_{};
    uint32_t revision_ = 0;
    PoolDeviceFeedback state_{};
    uint32_t retryAt_ = 0;
};
class DigitalRelayDriver final : public PoolDriverBase {
public:
    void tick(uint32_t now, bool writesEnabled) override;
};
class DiscreteSpeedDriver final : public PoolDriverBase {
public:
    void tick(uint32_t now, bool writesEnabled) override;
private:
    bool cleared_ = false;
    uint32_t readyAt_ = 0;
};
class AnalogSetpointDriver final : public PoolDriverBase {
public:
    void tick(uint32_t now, bool writesEnabled) override;
};
class SerialDeviceDriver final : public PoolDriverBase {
public:
    void applyTarget(const PoolDeviceTarget&, uint32_t revision) override;
    void tick(uint32_t now, bool writesEnabled) override;
private:
    enum class Operation : uint8_t { None, Setpoint, Run, Status, Feedback };
    bool submit_(Operation, uint32_t now);
    void consume_(const ModbusResponse&, uint32_t now);
    uint16_t transaction_ = MODBUS_TRANSACTION_INVALID;
    Operation operation_ = Operation::None;
    PoolDeviceTarget sentTarget_{};
    uint32_t sentRevision_ = 0;
    uint32_t speedRevision_ = 0;
    uint32_t nextPollAt_ = 0;
    uint32_t statusAt_ = 0;
    uint32_t levelAt_ = 0;
    bool statusValid_ = false;
    bool levelValid_ = false;
    bool pollLevel_ = false;
    bool frozen_ = false;
};

class PoolDeviceDriver {
public:
    IPoolDeviceDriver& configure(PoolControlKind kind);
    IPoolDeviceDriver& get();
private:
    std::variant<DigitalRelayDriver, DiscreteSpeedDriver, AnalogSetpointDriver, SerialDeviceDriver> driver_{};
};

bool validatePoolDriverConfig(const PoolDriverConfig& config);
bool validatePoolTarget(const PoolDriverConfig&, const PoolDeviceTarget&);

float poolCalibratedFlow(const PoolDriverConfig&, float setpoint);
