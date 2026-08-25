#pragma once

struct AppContext;
struct BoardSpec;
struct DomainSpec;

struct ProductIdentity {
    const char* productName = nullptr;
    const char* mdnsName = nullptr;
    const char* firmwareVersion = nullptr;
    const char* runtimeTopicRoot = nullptr;
};

struct FirmwareProfile {
    const char* name = nullptr;
    const BoardSpec* board = nullptr;
    const DomainSpec* domain = nullptr;
    ProductIdentity identity{};
    void (*setup)(AppContext&) = nullptr;
    void (*loop)(AppContext&) = nullptr;
};
