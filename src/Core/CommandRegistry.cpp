/**
 * @file CommandRegistry.cpp
 * @brief Implementation file.
 */
#include "CommandRegistry.h"
#include "Core/ErrorCodes.h"
#include "Core/SnprintfCheck.h"
#include <cstring>
#include <cstdio>
#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::Core)
#include "Core/ModuleLog.h"
#undef snprintf
#define snprintf(OUT, LEN, FMT, ...) \
    FLOW_SNPRINTF_CHECKED_MODULE(LOG_MODULE_ID, OUT, LEN, FMT, ##__VA_ARGS__)

static bool isJsonObjectReply_(const char* s, size_t len)
{
    if (!s || len == 0) return false;
    size_t i = 0;
    while (i < len) {
        const char c = s[i];
        if (c == '\0') return false;
        if (c != ' ' && c != '\t' && c != '\r' && c != '\n') {
            return c == '{';
        }
        ++i;
    }
    return false;
}

static const char* resolveCommandAlias_(const char* cmd)
{
    if (!cmd) return cmd;
    if (strcmp(cmd, "ntp.resync") == 0) return "time.resync";
    if (strcmp(cmd, "pool.write") == 0) return "pooldevice.write";
    if (strcmp(cmd, "pool.uptime.reset") == 0) return "pooldevice.uptime.reset";
    if (strcmp(cmd, "pool.uptime.reset_all") == 0) return "pooldevice.uptime.reset_all";
    if (strcmp(cmd, "poollogic.light.write") == 0) return "poollogic.lights.write";
    if (strcmp(cmd, "poollogic.light.toggle") == 0) return "poollogic.lights.toggle";
    if (strcmp(cmd, "poollogic.swg.write") == 0) return "poollogic.chlorine_generator.write";
    if (strcmp(cmd, "poollogic.swg.toggle") == 0) return "poollogic.chlorine_generator.toggle";
    return cmd;
}

bool CommandRegistry::registerHandler(const char* cmd, CommandHandler fn, void* userCtx) {
    if (!cmd || !fn) return false;
    if (count >= MAX_COMMANDS) {
        LOGW("Command registry full, dropping cmd=%s count=%u capacity=%u",
             cmd,
             (unsigned)count,
             (unsigned)MAX_COMMANDS);
        return false;
    }

    for (uint8_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].cmd, cmd) == 0) {
            LOGW("Command registry duplicate cmd=%s", cmd);
            return false;
        }
    }

    entries[count++] = {cmd, fn, userCtx};
    return true;
}

bool CommandRegistry::execute(const char* cmd, const char* json, const char* args, char* reply, size_t replyLen) {
    if (!cmd) {
        if (reply && replyLen) {
            if (!writeErrorJson(reply, replyLen, ErrorCode::UnknownCmd, "command")) {
                snprintf(reply, replyLen, "{\"ok\":false}");
            }
        }
        return false;
    }
    const char* resolvedCmd = resolveCommandAlias_(cmd);
    for (uint8_t i = 0; i < count; ++i) {
        if (strcmp(entries[i].cmd, resolvedCmd) == 0) {
            CommandRequest req{resolvedCmd, json, args};
            const bool ok = entries[i].fn(entries[i].userCtx, req, reply, replyLen);
            if (reply && replyLen) {
                if (!isJsonObjectReply_(reply, replyLen)) {
                    if (!writeErrorJson(reply, replyLen, ErrorCode::CmdHandlerFailed, "command.reply")) {
                        snprintf(reply, replyLen, "{\"ok\":false}");
                    }
                    return false;
                }
            }
            return ok;
        }
    }
    if (reply && replyLen) {
        if (!writeErrorJson(reply, replyLen, ErrorCode::UnknownCmd, "command")) {
            snprintf(reply, replyLen, "{\"ok\":false}");
        }
    }
    return false;
}
