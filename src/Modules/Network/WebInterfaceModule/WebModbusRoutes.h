#pragma once
/**
 * @brief Authenticated Flow.io Web routes for the Modbus TCP/RS485 gateway.
 *
 * The parent WebInterfaceModule installs its normal auth middleware after
 * registering these routes. POST is additionally restricted to admins.
 */
class PsramWebServer;
class ModbusTcpServerModule;
namespace WebModbusRoutes {
void registerRoutes(PsramWebServer& server, ModbusTcpServerModule* gateway);
}
