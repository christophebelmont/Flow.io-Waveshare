#pragma once
/**
 * @brief Arduino Network TCP socket adapter for Flow.io Modbus TCP gateway.
 *
 * Call begin() after networking and the ModbusMasterService are ready, and
 * tick() from ONE network task. No UART access and no cached register values.
 * The serial profile resolver must return the selected Flow.io device profile
 * or the manually configured profile for the requested port.
 */
#include <Arduino.h>
#include <WiFi.h>
#include <lwip/sockets.h>
#include <errno.h>
#include <esp_heap_caps.h>
#include "Core/Log.h"
#include "Core/LogModuleIds.h"
#include "ModbusTcpBridge.h"
#include "ModbusTcpSocketHealth.h"
#include "ModbusTcpSessions.h"
#include "ModbusTcpResolverRetry.h"

namespace ModbusTcpServer {
class SocketServer {
    struct FrameStorage {
        uint8_t rx[kMaxClients][ModbusTcpAdu::kMaxAdu]{};
        uint8_t tx[kMaxClients][ModbusTcpAdu::kMaxAdu]{};
    };
    static_assert(sizeof(FrameStorage) == 2U * kMaxClients * ModbusTcpAdu::kMaxAdu,
                  "Unexpected TCP frame storage overhead");
public:
    using ResolveLine = bool (*)(void* context, uint8_t portIndex,
                                uint8_t unitId, SerialLineProfile& out,
                                uint8_t& rtuUnitId);
    using ResolveLineDetailed = ResolveStatus (*)(void*, uint8_t, uint8_t,
                                                  SerialLineProfile&, uint8_t&);

    SocketServer()
        : servers_{WiFiServer(kPorts[0]), WiFiServer(kPorts[1]), WiFiServer(kPorts[2])},
          bridges_{BridgeSlot(201), BridgeSlot(202), BridgeSlot(203),
                   BridgeSlot(204), BridgeSlot(205), BridgeSlot(206)} {}

    void configure(uint8_t port, const PortConfig& config) {
        if (validConfig(port, config)) configs_[port] = config;
    }

    // Fail closed when external RAM is unavailable. No internal heap fallback.
    // The allocation is retained across all Wi-Fi / network reconnections.
    bool begin(const ModbusMasterService& service, ResolveLine resolver,
               void* resolverContext) {
        if (started_) return true;
        if (!frameStorage_) {
            const size_t beforeInternal = heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            const size_t beforeLargest = heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
            void* memory = heap_caps_calloc(1U, sizeof(FrameStorage),
                                            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
            if (!memory) {
                Serial.printf("[MODBUS-PSRAM] allocation_failed bytes=%u internal_free=%u psram_free=%u\n",
                              (unsigned)sizeof(FrameStorage), (unsigned)beforeInternal,
                              (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
                return false;
            }
            frameStorage_ = static_cast<FrameStorage*>(memory);
            sessions_.attachFrames(frameStorage_->rx);
            for (uint8_t i = 0U; i < kMaxClients; ++i)
                bridges_[i].attachOutput(frameStorage_->tx[i]);
            Serial.printf("[MODBUS-PSRAM] ready rx_bytes=%u tx_bytes=%u total=%u internal_before=%u internal_after=%u largest_before=%u largest_after=%u psram_free=%u\n",
                          (unsigned)sizeof(frameStorage_->rx), (unsigned)sizeof(frameStorage_->tx),
                          (unsigned)sizeof(FrameStorage), (unsigned)beforeInternal,
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                          (unsigned)beforeLargest,
                          (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT),
                          (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        }
        service_ = &service;
        resolver_ = resolver;
        resolverDetailed_ = nullptr;
        resolverContext_ = resolverContext;
        for (uint8_t port = 0; port < kPortCount; ++port)
            if (configs_[port].enabled) servers_[port].begin();
        started_ = true;
        return true;
    }

    bool beginDetailed(const ModbusMasterService& service,
                       ResolveLineDetailed resolver, void* resolverContext) {
        if (!resolver) return false;
        if (!begin(service, static_cast<ResolveLine>(nullptr), resolverContext))
            return false;
        resolverDetailed_ = resolver;
        return true;
    }

    void stop() {
        for (uint8_t slot = 0; slot < kMaxClients; ++slot) close_(slot);
        for (auto& server : servers_) server.end();
        started_ = false;
        service_ = nullptr;
    }

    /** Nonblocking best-effort network servicing, with bounded read work.
     * A partial TCP write is resumed on a subsequent tick.
     */
    // Slow/failed requests only. Limit Web logs per port so a faulty client
    // cannot flood the asynchronous LogHub or delay the shared RS485 scheduler.
    static constexpr uint32_t kTraceSlowMs = 1000U;
    static constexpr uint32_t kTraceLogIntervalMs = 1000U;
    void trace_(uint8_t slot, const char* stage, uint32_t nowMs, int result = 0) {
#if defined(FLOW_MODBUS_VERBOSE_DIAGNOSTICS) && FLOW_MODBUS_VERBOSE_DIAGNOSTICS
        if (!traceActive_[slot]) return;
        const uint8_t port = slot / kClientsPerPort;
        if (lastTraceLogMs_[port] != 0U &&
            uint32_t(nowMs - lastTraceLogMs_[port]) < kTraceLogIntervalMs) {
            ++traceSuppressed_[port];
            return;
        }
        lastTraceLogMs_[port] = nowMs ? nowMs : 1U;
        ::Log::warn((LogModuleId)LogModuleIdValue::ModbusTcpServerModule,
            "[MODBUS-TRACE] port=%u tid=%u unit=%u fc=%u addr=%u qty=%u stage=%s age=%lu q=%lu rtu=%lu rc=%d",
            (unsigned)kPorts[port], (unsigned)traceTid_[slot],
            (unsigned)traceUnit_[slot], (unsigned)traceFc_[slot],
            (unsigned)traceAddr_[slot], (unsigned)traceQty_[slot], stage,
            (unsigned long)(nowMs - traceStarted_[slot]),
            (unsigned long)bridges_[slot].queueWaitMs(),
            (unsigned long)bridges_[slot].rtuResponseMs(), result);
#else
        // Avoid Web log pressure and slow-response warnings in normal builds.
        (void)slot; (void)stage; (void)nowMs; (void)result;
#endif
    }

    void tick(uint32_t nowMs) {
        if (!started_ || !service_) return;
        // Reap closed sockets BEFORE admitting new clients: otherwise the
        // first reconnection can be rejected despite a released peer.
        probeClosedPeers_(nowMs);
        for (uint8_t port = 0; port < kPortCount; ++port) {
            if (!configs_[port].enabled) continue;
            WiFiClient incoming = servers_[port].accept();
            if (incoming) {
                int8_t slot = sessions_.accept(port);
                if (slot < 0 && reclaimIdleSession_(port, nowMs))
                    slot = sessions_.accept(port);
                if (slot < 0) {
                    ++stats_[port].connectionsRejected;
                    incoming.stop();
                } else {
                    clients_[slot] = incoming;
                    peerReadClosed_[slot] = false;
                    configureKeepAlive_(slot);
                    lastReadProgressMs_[slot] = nowMs;
                    lastConnectionCheckMs_[slot] = nowMs;
                    stats_[port].activeClients = sessions_.active(port);
                    if (stats_[port].activeClients > stats_[port].maxActiveClients)
                        stats_[port].maxActiveClients = stats_[port].activeClients;
                }
            }
        }
        for (uint8_t slot = 0; slot < kMaxClients; ++slot) {
            const uint8_t port = slot / kClientsPerPort;
            WiFiClient& client = clients_[slot];
            if (!sessions_.occupied(slot)) continue;
            // Peer liveness is tested by a bounded raw recv(MSG_PEEK)
            // before admission. Avoid NetworkClient::connected(), which
            // may not reliably mark orderly FIN as disconnected.
            if (client.fd() < 0) { closePeer_(slot); continue; }
            // A peer may hold a scarce per-port slot by sending only part
            // of an MBAP frame. Close incomplete frames with no progress,
            // but preserve normal long-lived idle Modbus TCP connections.
            if (sessions_.hasPartialRequest(slot) &&
                uint32_t(nowMs - lastReadProgressMs_[slot]) >= kPartialFrameTimeoutMs) {
                close_(slot);
                continue;
            }
            BridgeSlot& bridge = bridges_[slot];
            bridge.poll(nowMs);
            if (bridge.state() == BridgeSlot::State::Ready) {
                if (!writing_[slot]) {
                    writing_[slot] = true;
                    lastWriteProgressMs_[slot] = nowMs;
                }
                const size_t remaining = bridge.outputSize() - sent_[slot];
                if (remaining) {
                    // Arduino-ESP32 NetworkClient does not override
                    // Print::availableForWrite(), which always returns zero.
                    // Using it as a readiness gate prevented *every* Modbus
                    // response (including exceptions) from being sent.
                    //
                    // Use a bounded, nonblocking socket send rather than
                    // NetworkClient::write(), whose select/retry loop can
                    // block the Modbus task when a peer stops reading.
                    const int fd = client.fd();
                    if (fd < 0) {
                        ++stats_[port].txErrors;
                        close_(slot);
                        continue;
                    }
                    const size_t count = remaining < kMaxTxBytesPerTick
                        ? remaining : kMaxTxBytesPerTick;
                    const int wrote = ::send(fd, bridge.output() + sent_[slot],
                                             count, MSG_DONTWAIT);
                    if (wrote > 0) {
                        sent_[slot] += size_t(wrote);
                        stats_[port].tcpBytesSent += uint32_t(wrote);
                        lastWriteProgressMs_[slot] = nowMs;
                    } else if (wrote < 0 && errno != EAGAIN &&
                               errno != EWOULDBLOCK && errno != EINTR) {
                        ++stats_[port].txErrors;
                        close_(slot);
                        continue;
                    } else {
                        ++stats_[port].txWouldBlock;
                    }
                }
                if (sent_[slot] == bridge.outputSize()) {
                    accountOutcome_(port, bridge, true);
                    ++stats_[port].completed;
                    const uint32_t elapsed = bridge.elapsedMs(nowMs);
                    stats_[port].responseTimeTotalMs += elapsed;
                    if (elapsed > stats_[port].responseTimeMaxMs)
                        stats_[port].responseTimeMaxMs = elapsed;
                    if (bridge.resultCode() != MODBUS_RESULT_OK ||
                        elapsed >= kTraceSlowMs)
                        trace_(slot, "tcp_tx_complete", nowMs, int(bridge.resultCode()));
                    traceActive_[slot] = false;
                    bridge.reset();
                    sessions_.clearRequest(slot);
                    sent_[slot] = 0U;
                    writing_[slot] = false;
                } else if (uint32_t(nowMs - lastWriteProgressMs_[slot]) >= kWriteStallTimeoutMs) {
                    // Release a slot held by a peer that has stopped reading.
                    trace_(slot, "tcp_tx_stall", nowMs);
                    close_(slot);
                    continue;
                }
            } else if (bridge.state() == BridgeSlot::State::Silent) {
                accountOutcome_(port, bridge, false);
                ++stats_[port].silentFailures;
                trace_(slot, "silent_failure", nowMs, int(bridge.resultCode()));
                traceActive_[slot] = false;
                bridge.reset();
                sessions_.clearRequest(slot);
                sent_[slot] = 0U;
                writing_[slot] = false;
            }
            if (bridge.state() != BridgeSlot::State::Idle) continue;
            if (resolverRetry_[slot].pending()) {
                // Leave the exact Modbus TCP ADU buffered until resolved.
                // Only the first receipt counts as a TCP request.
                if (!resolverRetry_[slot].due(nowMs)) continue;
                ModbusTcpAdu::Request pending{};
                if (!sessions_.request(slot, pending)) {
                    close_(slot);
                    continue;
                }
                dispatchRequest_(slot, pending, nowMs);
                continue;
            }
            // Preserve the reply for a half-closed TCP peer, then release
            // immediately after the in-flight Modbus transaction finishes.
            if (canReleaseAfterReadClose(peerReadClosed_[slot], true, writing_[slot])) {
                closePeer_(slot);
                continue;
            }
            // One bounded, nonblocking recv per active socket per tick.
            // Previously, available() performed a lwIP FIONREAD ioctl for
            // EVERY byte of the Modbus ADU (up to 260 calls/connection),
            // starving HTTP and ICMP handling under sustained traffic.
            //
            // Read only up to the exact current MBAP/ADU boundary: bytes
            // from the next pipelined TCP request stay in the socket until
            // this request is answered through the shared RS485 scheduler.
            const size_t needed = sessions_.bytesNeeded(slot);
            if (!needed) continue;
            uint8_t input[kMaxRxBytesPerTick]{};
            const size_t readLength = needed < sizeof(input) ? needed : sizeof(input);
            const int received = ::recv(client.fd(), input, readLength, MSG_DONTWAIT);
            const int socketErrno = received < 0 ? errno : 0;
            if (received == 0) {
                // FIN even if Arduino NetworkClient still reports connected.
                closePeer_(slot);
                continue;
            }
            if (received < 0) {
                if (socketErrno != EAGAIN && socketErrno != EWOULDBLOCK &&
                    socketErrno != EINTR) {
                    ++stats_[port].rxErrors;
                    closePeer_(slot);
                }
                continue;
            }
            lastReadProgressMs_[slot] = nowMs;
            stats_[port].tcpBytesReceived += uint32_t(received);
            size_t consumed = 0U;
            const auto status = sessions_.feed(slot, input, size_t(received), consumed);
            if (status == TcpSessions::FeedResult::Invalid ||
                consumed != size_t(received)) {
                close_(slot);
                continue;
            }
            if (status != TcpSessions::FeedResult::Complete) continue;
            ModbusTcpAdu::Request request{};
            if (!sessions_.request(slot, request)) {
                ModbusTcpAdu::Request header{};
                if (!sessions_.headerRequest(slot, header)) {
                    close_(slot);
                    continue;
                }
                ++stats_[port].received;
                const bool supported = header.function == 0x03U ||
                    header.function == 0x04U || header.function == 0x06U ||
                    header.function == 0x10U;
                bridge.rejectException(header, configs_[port].errorMode,
                                       supported ? 0x03U : 0x01U, nowMs);
                continue;
            }
            ++stats_[port].received;
            traceActive_[slot] = true;
            traceStarted_[slot] = nowMs;
            traceTid_[slot] = request.transactionId;
            traceUnit_[slot] = request.unitId;
            traceFc_[slot] = request.function;
            traceAddr_[slot] = request.address;
            traceQty_[slot] = request.count;
            resolverRetry_[slot].begin(nowMs);
            dispatchRequest_(slot, request, nowMs);
            // One outstanding request per connection; the next pipelined
            // ADU remains buffered by lwIP until its predecessor is answered.
        }
    }

    const PortStats& stats(uint8_t port) const { return stats_[port < kPortCount ? port : 0U]; }
    uint32_t suppressedTraceCount(uint8_t port) const {
        return port < kPortCount ? traceSuppressed_[port] : 0U;
    }
    size_t psramFrameBytes() const { return frameStorage_ ? sizeof(FrameStorage) : 0U; }
    bool frameStorageReady() const { return frameStorage_ != nullptr; }
private:
    void dispatchRequest_(uint8_t slot, const ModbusTcpAdu::Request& request,
                          uint32_t nowMs) {
        const uint8_t port = slot / kClientsPerPort;
        BridgeSlot& bridge = bridges_[slot];
        const bool previouslyBusy = resolverRetry_[slot].pending();
        SerialLineProfile line{};
        uint8_t targetUnitId = request.unitId;
        const ResolveStatus status = resolverDetailed_
            ? resolverDetailed_(resolverContext_, port, request.unitId, line, targetUnitId)
            : (resolver_ && resolver_(resolverContext_, port, request.unitId, line, targetUnitId)
                ? ResolveStatus::Ready : ResolveStatus::Invalid);
        if (status == ResolveStatus::Busy) ++stats_[port].resolverBusyAttempts;
        const ResolveDecision decision = resolverRetry_[slot].consider(status, nowMs);
        if (decision == ResolveDecision::Retry) return;
        if (decision != ResolveDecision::Ready) {
            ++stats_[port].resolverFailures;
            stats_[port].lastResolverAtMs = nowMs;
            stats_[port].lastResolverUnitId = request.unitId;
            stats_[port].lastResolverFunction = request.function;
            stats_[port].lastResolverAddress = request.address;
            if (decision == ResolveDecision::BusyTimeout) {
                ++stats_[port].resolverBusyTimeouts;
                stats_[port].lastResolverCause = 1U;
                trace_(slot, "resolver_lock_timeout", nowMs);
            } else {
                ++stats_[port].resolverInvalidFailures;
                stats_[port].lastResolverCause = 2U;
                trace_(slot, "resolver_invalid_profile", nowMs);
            }
            // A failed lookup is never forwarded to the RS485 scheduler.
            // In Silent mode the bridge emits no response.
            bridge.reject(request, configs_[port].errorMode,
                          MODBUS_RESULT_NOT_READY, nowMs);
            return;
        }
        if (previouslyBusy) ++stats_[port].resolverBusyRecovered;
        // Shared ModbusMasterService remains the sole RS485 scheduler.
        if (!bridge.submit(*service_, request, line, configs_[port].errorMode,
                           configs_[port].transactionTimeoutMs, nowMs, targetUnitId)) {
            ++stats_[port].queueRejected;
            trace_(slot, "scheduler_submit_failed", nowMs);
            bridge.reject(request, configs_[port].errorMode,
                          MODBUS_RESULT_INVALID_ARGUMENT, nowMs);
        }
    }

    void accountOutcome_(uint8_t port, const BridgeSlot& bridge, bool replied) {
        if (bridge.hasSerialTiming()) {
            const uint32_t queueMs = bridge.queueWaitMs();
            const uint32_t responseMs = bridge.rtuResponseMs();
            ++stats_[port].rtuSamples;
            stats_[port].queueWaitTotalMs += queueMs;
            stats_[port].rtuResponseTotalMs += responseMs;
            if (queueMs > stats_[port].queueWaitMaxMs)
                stats_[port].queueWaitMaxMs = queueMs;
            if (responseMs > stats_[port].rtuResponseMaxMs)
                stats_[port].rtuResponseMaxMs = responseMs;
        }
        const ModbusResultCode result = bridge.resultCode();
        if (result == MODBUS_RESULT_TIMEOUT || result == MODBUS_RESULT_EXPIRED)
            ++stats_[port].timeouts;
        if (result == MODBUS_RESULT_CRC_ERROR)
            ++stats_[port].crcErrors;
        if (result == MODBUS_RESULT_QUEUE_FULL)
            ++stats_[port].queueRejected;
        if (replied && result != MODBUS_RESULT_OK)
            ++stats_[port].exceptionsSent;
    }
    static constexpr size_t kMaxTxBytesPerTick = 128U;
    static constexpr size_t kMaxRxBytesPerTick = 64U;
    static constexpr uint32_t kConnectionCheckMs = 250U;
    // A dead peer which never sent FIN/RST is detected by lwIP keepalive.
    // Active clients are never evicted merely for being idle.
    static constexpr int kKeepAliveIdleSec = 10;
    static constexpr int kKeepAliveIntervalSec = 5;
    static constexpr int kKeepAliveProbes = 3;
    // Wi-Fi/OTA reboot can orphan persistent TCP sockets without a FIN/RST.
    // Evict an idle slot only under admission pressure, never an in-flight
    // Modbus transaction. This does not run extra socket polls each tick.
    static constexpr uint32_t kIdleReclaimMs = 30000U;
    static constexpr uint32_t kWriteStallTimeoutMs = 5000U;
    static constexpr uint32_t kPartialFrameTimeoutMs = 10000U;
    void configureKeepAlive_(uint8_t slot) {
        const int fd = clients_[slot].fd();
        if (fd < 0) return;
        const int enable = 1;
        // The socket options are supported by ESP-IDF's lwIP. Keep a
        // diagnostic counter if the target framework rejects them.
        bool configured = ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE,
                                       &enable, sizeof(enable)) == 0;
#if defined(TCP_KEEPIDLE) && defined(TCP_KEEPINTVL) && defined(TCP_KEEPCNT)
        const int idle = kKeepAliveIdleSec;
        const int interval = kKeepAliveIntervalSec;
        const int probes = kKeepAliveProbes;
        configured = (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPIDLE,
                                   &idle, sizeof(idle)) == 0) && configured;
        configured = (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPINTVL,
                                   &interval, sizeof(interval)) == 0) && configured;
        configured = (::setsockopt(fd, IPPROTO_TCP, TCP_KEEPCNT,
                                   &probes, sizeof(probes)) == 0) && configured;
#else
        configured = false; // SO_KEEPALIVE still active with system defaults.
#endif
        if (!configured) ++stats_[slot / kClientsPerPort].keepaliveSetupFailures;
    }

    void closePeer_(uint8_t slot) {
        ++stats_[slot / kClientsPerPort].peerClosed;
        close_(slot);
    }

    void probeClosedPeers_(uint32_t nowMs) {
        for (uint8_t slot = 0; slot < kMaxClients; ++slot) {
            if (!sessions_.occupied(slot)) continue;
            const uint8_t port = slot / kClientsPerPort;
            const int fd = clients_[slot].fd();
            if (fd < 0) { closePeer_(slot); continue; }
            if (uint32_t(nowMs - lastConnectionCheckMs_[slot]) < kConnectionCheckMs)
                continue;
            lastConnectionCheckMs_[slot] = nowMs;
            uint8_t byte = 0U;
            // MSG_PEEK does not consume an MBAP byte from a pipelined frame.
            // A nonblocking EOF (recv==0) means FIN, not an idle socket.
            const int result = ::recv(fd, &byte, 1U, MSG_PEEK | MSG_DONTWAIT);
            const int socketErrno = result < 0 ? errno : 0;
            const auto state = classifySocketPeerProbe(result, socketErrno);
            if (state == SocketPeerHealth::Error) {
                ++stats_[slot / kClientsPerPort].rxErrors;
                closePeer_(slot);
            } else if (state == SocketPeerHealth::ReadClosed) {
                peerReadClosed_[slot] = true;
                if (canReleaseAfterReadClose(peerReadClosed_[slot],
                        bridges_[slot].state() == BridgeSlot::State::Idle &&
                            !resolverRetry_[slot].pending(),
                        writing_[slot]))
                    closePeer_(slot);
            }
        }
    }

    bool reclaimIdleSession_(uint8_t port, uint32_t nowMs) {
        int8_t victim = -1;
        uint32_t oldestIdleMs = 0U;
        const uint8_t first = port * kClientsPerPort;
        for (uint8_t slot = first; slot < first + kClientsPerPort; ++slot) {
            if (!sessions_.occupied(slot) ||
                bridges_[slot].state() != BridgeSlot::State::Idle ||
                writing_[slot] || sessions_.hasPartialRequest(slot))
                continue;
            const uint32_t idleMs = uint32_t(nowMs - lastReadProgressMs_[slot]);
            if (idleMs < kIdleReclaimMs) continue;
            if (victim < 0 || idleMs > oldestIdleMs) {
                victim = int8_t(slot);
                oldestIdleMs = idleMs;
            }
        }
        if (victim < 0) return false;
        close_(uint8_t(victim));
        ++stats_[port].idleReclaimed;
        return true;
    }
    void close_(uint8_t slot) {
        resolverRetry_[slot].reset();
        if (traceActive_[slot]) trace_(slot, "socket_closed", millis());
        traceActive_[slot] = false;
        bridges_[slot].reset();
        clients_[slot].stop();
        sessions_.release(slot);
        sent_[slot] = 0U;
        writing_[slot] = false;
        lastWriteProgressMs_[slot] = 0U;
        lastReadProgressMs_[slot] = 0U;
        lastConnectionCheckMs_[slot] = 0U;
        peerReadClosed_[slot] = false;
        const uint8_t port = slot / kClientsPerPort;
        stats_[port].activeClients = sessions_.active(port);
    }
    WiFiServer servers_[kPortCount];
    WiFiClient clients_[kMaxClients];
    TcpSessions sessions_{};
    BridgeSlot bridges_[kMaxClients];
    PortConfig configs_[kPortCount]{};
    PortStats stats_[kPortCount]{};
    uint32_t lastTraceLogMs_[kPortCount]{};
    uint32_t traceSuppressed_[kPortCount]{};
    size_t sent_[kMaxClients]{};
    uint32_t lastWriteProgressMs_[kMaxClients]{};
    uint32_t lastReadProgressMs_[kMaxClients]{};
    uint32_t lastConnectionCheckMs_[kMaxClients]{};
    bool peerReadClosed_[kMaxClients]{};
    bool writing_[kMaxClients]{};
    bool traceActive_[kMaxClients]{};
    ResolverRetryPolicy resolverRetry_[kMaxClients]{};
    uint32_t traceStarted_[kMaxClients]{};
    uint16_t traceTid_[kMaxClients]{};
    uint16_t traceAddr_[kMaxClients]{};
    uint16_t traceQty_[kMaxClients]{};
    uint8_t traceUnit_[kMaxClients]{};
    uint8_t traceFc_[kMaxClients]{};
    const ModbusMasterService* service_ = nullptr;
    ResolveLine resolver_ = nullptr;
    ResolveLineDetailed resolverDetailed_ = nullptr;
    void* resolverContext_ = nullptr;
    bool started_ = false;
    FrameStorage* frameStorage_ = nullptr;
};
} // namespace ModbusTcpServer
