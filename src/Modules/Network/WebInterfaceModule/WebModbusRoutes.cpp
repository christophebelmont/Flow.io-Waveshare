#include "WebModbusRoutes.h"

#include "PsramWebServer.h"
#include "Modules/Network/ModbusTcpServerModule/ModbusTcpServerModule.h"
#include "Modules/PoolDeviceModule/PoolDeviceModuleDataModel.h"

#include <Arduino.h>
#include <stdint.h>

namespace {
void printJsonText_(Print& stream, const char* text)
{
    stream.print('"');
    if (text) {
        for (const char* p = text; *p; ++p) {
            switch (*p) {
                case '"': stream.print("\\\""); break;
                case '\\': stream.print("\\\\"); break;
                case '\n': stream.print("\\n"); break;
                case '\r': stream.print("\\r"); break;
                case '\t': stream.print("\\t"); break;
                default:
                    if (uint8_t(*p) < 32U) stream.print('?');
                    else stream.print(*p);
                    break;
            }
        }
    }
    stream.print('"');
}

} // namespace

namespace WebModbusRoutes {
void registerRoutes(PsramWebServer& server, ModbusTcpServerModule* gateway)
{
    if (!gateway) return;
    server.on("/api/modbus/ports", HTTP_GET, [gateway](AsyncWebServerRequest* request) {
        AsyncResponseStream* response = request->beginResponseStream("application/json");
        if (!response) {
            request->send(503, "application/json", "{\"ok\":false}");
            return;
        }
        response->addHeader("Cache-Control", "no-store");
        ModbusMasterStats master{};
        (void)gateway->schedulerStats(master);
        response->printf(
            "{\"ok\":true,\"running\":%s,\"restart_required\":true,"
            "\"scheduler\":{\"queue\":%u,\"peak\":%u,\"rejected\":%lu},\"ports\":[",
            gateway->running() ? "true" : "false",
            (unsigned)master.queued, (unsigned)master.queuedPeak,
            (unsigned long)master.queueRejected);
        for (uint8_t i = 0; i < ModbusTcpServer::kPortCount; ++i) {
            ModbusTcpServer::PortConfig cfg{};
            if (!gateway->readPort(i, cfg)) continue;
            if (i) response->print(',');
            response->printf(
                "{\"index\":%u,\"port\":%u,\"enabled\":%s,\"source\":%u,"
                "\"device_id\":%u,\"baud\":%lu,\"data_bits\":8,\"parity\":%u,"
                "\"stop_bits\":%u,\"timeout_ms\":%lu,\"error_mode\":%u",
                (unsigned)i, (unsigned)cfg.tcpPort,
                cfg.enabled ? "true" : "false",
                (unsigned)cfg.serialSource,
                (unsigned)cfg.deviceId,
                (unsigned long)cfg.manualLine.baud,
                (unsigned)cfg.manualLine.parity,
                (unsigned)cfg.manualLine.stopBits,
                (unsigned long)cfg.transactionTimeoutMs,
                (unsigned)cfg.errorMode);
            // Report listener settings separately from staged ConfigStore.
            // A cfg/set command does not reconfigure active sockets until reboot.
            ModbusTcpServer::PortConfig active{};
            const bool hasActive = gateway->readActivePort(i, active);
            const bool pendingRestart = hasActive && (
                active.enabled != cfg.enabled ||
                active.serialSource != cfg.serialSource ||
                active.deviceId != cfg.deviceId ||
                active.errorMode != cfg.errorMode ||
                active.transactionTimeoutMs != cfg.transactionTimeoutMs ||
                (cfg.serialSource == ModbusTcpServer::SerialSource::Manual &&
                    (active.manualLine.baud != cfg.manualLine.baud ||
                     active.manualLine.parity != cfg.manualLine.parity ||
                     active.manualLine.stopBits != cfg.manualLine.stopBits)));
            response->printf(
                ",\"active_enabled\":%s,\"active_error_mode\":%u,"
                "\"active_device_id\":%u,\"pending_restart\":%s",
                (hasActive && active.enabled) ? "true" : "false",
                (unsigned)active.errorMode,
                (unsigned)active.deviceId,
                pendingRestart ? "true" : "false");
            const ModbusTcpServer::PortStats& stat = gateway->portStats(i);
            const uint32_t averageMs = stat.completed ?
                stat.responseTimeTotalMs / stat.completed : 0U;
            const uint32_t queueAverageMs = stat.rtuSamples ?
                stat.queueWaitTotalMs / stat.rtuSamples : 0U;
            const uint32_t rtuAverageMs = stat.rtuSamples ?
                stat.rtuResponseTotalMs / stat.rtuSamples : 0U;
            response->printf(
                ",\"stats\":{\"clients\":%u,\"peak_clients\":%u,"
                "\"requests\":%lu,\"responses\":%lu,\"exceptions\":%lu,"
                "\"silent\":%lu,\"timeouts\":%lu,\"crc_errors\":%lu,"
                "\"queue_rejected\":%lu,\"connections_rejected\":%lu,"
                "\"idle_reclaimed\":%lu,\"peer_closed\":%lu,"
                "\"keepalive_setup_failures\":%lu,"
                "\"resolver_failures\":%lu,\"tx_wouldblock\":%lu,"
                "\"tx_errors\":%lu,\"rx_errors\":%lu,"
                "\"tcp_bytes_received\":%lu,\"tcp_bytes_sent\":%lu,"
                "\"tcp_avg_ms\":%lu,\"tcp_max_ms\":%lu,"
                "\"queue_avg_ms\":%lu,\"queue_max_ms\":%lu,"
                "\"rtu_avg_ms\":%lu,\"rtu_max_ms\":%lu}}",
                (unsigned)stat.activeClients, (unsigned)stat.maxActiveClients,
                (unsigned long)stat.received, (unsigned long)stat.completed,
                (unsigned long)stat.exceptionsSent, (unsigned long)stat.silentFailures,
                (unsigned long)stat.timeouts, (unsigned long)stat.crcErrors,
                (unsigned long)stat.queueRejected,
                (unsigned long)stat.connectionsRejected,
                (unsigned long)stat.idleReclaimed,
                (unsigned long)stat.peerClosed,
                (unsigned long)stat.keepaliveSetupFailures,
                (unsigned long)stat.resolverFailures,
                (unsigned long)stat.txWouldBlock,
                (unsigned long)stat.txErrors,
                (unsigned long)stat.rxErrors,
                (unsigned long)stat.tcpBytesReceived,
                (unsigned long)stat.tcpBytesSent,
                (unsigned long)averageMs,
                (unsigned long)stat.responseTimeMaxMs,
                (unsigned long)queueAverageMs,
                (unsigned long)stat.queueWaitMaxMs,
                (unsigned long)rtuAverageMs,
                (unsigned long)stat.rtuResponseMaxMs);
        }
        response->print("],\"devices\":[");
        bool first = true;
        for (uint16_t id = 0; id < POOL_DEVICE_MAX; ++id) {
            SerialLineProfile line{};
            char label[32]{};
            uint8_t slaveAddress = 0U;
            if (!gateway->serialDeviceInfo(id, line, label, sizeof(label), &slaveAddress)) continue;
            if (!first) response->print(',');
            first = false;
            response->printf("{\"id\":%u,\"label\":", (unsigned)id);
            printJsonText_(*response, label);
            response->printf(",\"baud\":%lu,\"parity\":%u,\"stop_bits\":%u,\"rtu_unit_id\":%u}",
                             (unsigned long)line.baud, (unsigned)line.parity,
                             (unsigned)line.stopBits, (unsigned)slaveAddress);
        }
        response->print("]}");
        request->send(response);
    });

    // Configuration is edited only through the native ConfigStore API
    // (/api/flowcfg/apply) or MQTT cfg/set. The obsolete NVS blob writer
    // has been removed to prevent competing settings sources.

}
} // namespace WebModbusRoutes
