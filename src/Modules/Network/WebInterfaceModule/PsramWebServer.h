#pragma once

#include <ESPAsyncWebServer.h>
#include <WebHandlerImpl.h>
#include <esp_heap_caps.h>
#include <memory>
#include <new>

// Retain the library's routing and ownership semantics, changing only placement
// of route objects. URI strings, callback captures and list nodes remain owned
// and allocated by ESPAsyncWebServer.
class PsramWebServer final : public AsyncWebServer {
public:
    explicit PsramWebServer(uint16_t port) : AsyncWebServer(port) {}

    bool on(AsyncURIMatcher uri, WebRequestMethodComposite method,
            ArRequestHandlerFunction request,
            ArUploadHandlerFunction upload = nullptr,
            ArBodyHandlerFunction body = nullptr)
    {
        if (allocationFailed_) return false;
        std::unique_ptr<Route> route(new (std::nothrow) Route());
        if (!route) {
            allocationFailed_ = true;
            return false;
        }
        route->setUri(std::move(uri));
        route->setMethod(std::move(method));
        route->onRequest(std::move(request));
        route->onUpload(std::move(upload));
        route->onBody(std::move(body));
        addHandler(route.release());
        ++routeCount_;
        return true;
    }

    bool routesReady() const { return !allocationFailed_ && routeCount_ != 0; }
    size_t routeCount() const { return routeCount_; }
    size_t routeStorageBytes() const { return routeCount_ * sizeof(Route); }

    // Only used before listening and before attaching embedded WS/SSE handlers.
    void resetRoutes()
    {
        AsyncWebServer::reset();
        allocationFailed_ = false;
        routeCount_ = 0;
    }

private:
    class Route final : public AsyncCallbackWebHandler {
    public:
        static void* operator new(size_t size, const std::nothrow_t&) noexcept
        {
            return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        }
        static void operator delete(void* pointer) noexcept { heap_caps_free(pointer); }
        static void operator delete(void* pointer, const std::nothrow_t&) noexcept { heap_caps_free(pointer); }
    };
    bool allocationFailed_ = false;
    size_t routeCount_ = 0;
};
