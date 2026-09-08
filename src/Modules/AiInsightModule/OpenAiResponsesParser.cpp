/**
 * @file OpenAiResponsesParser.cpp
 * @brief OpenAI Responses API text and error parser.
 */

#include "Modules/AiInsightModule/OpenAiResponsesParser.h"

#include <stdio.h>
#include <string.h>

namespace {

bool writeText_(char* out, size_t outLen, const char* text)
{
    if (!out || outLen == 0U) return false;
    const int written = snprintf(out, outLen, "%s", text ? text : "");
    return written >= 0 && (size_t)written < outLen;
}

bool appendText_(char* out, size_t outLen, size_t& used, const char* text)
{
    if (!out || outLen == 0U || !text) return false;
    const size_t textLen = strlen(text);
    const size_t separatorLen = used > 0U ? 1U : 0U;
    if (used >= outLen || separatorLen > (outLen - used - 1U) ||
        textLen > (outLen - used - separatorLen - 1U)) {
        return false;
    }
    if (separatorLen != 0U) out[used++] = '\n';
    memcpy(out + used, text, textLen);
    used += textLen;
    out[used] = '\0';
    return true;
}

}  // namespace

namespace OpenAiResponsesParser {

bool extractApiError(JsonVariantConst root, char* errOut, size_t errOutLen)
{
    if (!errOut || errOutLen == 0U) return false;
    errOut[0] = '\0';
    const JsonObjectConst error = root["error"].as<JsonObjectConst>();
    if (error.isNull()) return false;

    const char* message = error["message"].as<const char*>();
    const char* code = error["code"].as<const char*>();
    const char* type = error["type"].as<const char*>();
    if (!message) message = "";
    if (!code) code = "";
    if (!type) type = "";
    if (message[0] == '\0' && code[0] == '\0' && type[0] == '\0') return false;

    // A truncated diagnostic remains useful and must not be replaced by the
    // generic HTTP status. Put the machine-readable discriminator first so it
    // survives even when the human-readable API message is unusually long.
    int written = 0;
    if (code[0] != '\0') {
        written = snprintf(errOut, errOutLen, "OpenAI [%s]: %s", code, message);
    } else if (type[0] != '\0') {
        written = snprintf(errOut, errOutLen, "OpenAI [%s]: %s", type, message);
    } else {
        written = snprintf(errOut, errOutLen, "OpenAI: %s", message);
    }
    return written >= 0;
}

bool parse(JsonVariantConst root,
           Result& out,
           char* textOut,
           size_t textOutLen,
           char* errOut,
           size_t errOutLen)
{
    out = Result{};
    if (textOut && textOutLen > 0U) textOut[0] = '\0';
    if (errOut && errOutLen > 0U) errOut[0] = '\0';
    if (!textOut || textOutLen == 0U) {
        writeText_(errOut, errOutLen, "OpenAI result storage unavailable");
        return false;
    }
    if (extractApiError(root, errOut, errOutLen)) return false;

    const char* responseStatus = root["status"].as<const char*>();
    if (!responseStatus) responseStatus = "";
    if (strcmp(responseStatus, "completed") != 0) {
        const char* incompleteReason =
            root["incomplete_details"]["reason"].as<const char*>();
        char detail[192]{};
        if (incompleteReason && incompleteReason[0] != '\0') {
            snprintf(detail,
                     sizeof(detail),
                     "OpenAI response is %s (%s)",
                     responseStatus[0] ? responseStatus : "not completed",
                     incompleteReason);
        } else {
            snprintf(detail,
                     sizeof(detail),
                     "OpenAI response is %s",
                     responseStatus[0] ? responseStatus : "not completed");
        }
        writeText_(errOut, errOutLen, detail);
        return false;
    }

    const char* responseId = root["id"].as<const char*>();
    const char* model = root["model"].as<const char*>();
    if (!responseId) responseId = "";
    if (!model) model = "";
    if (!writeText_(out.responseId, sizeof(out.responseId), responseId) ||
        !writeText_(out.model, sizeof(out.model), model)) {
        writeText_(errOut, errOutLen, "OpenAI response metadata is too long");
        return false;
    }

    size_t used = 0U;
    for (JsonObjectConst item : root["output"].as<JsonArrayConst>()) {
        const char* itemType = item["type"].as<const char*>();
        if (!itemType) itemType = "";
        if (strcmp(itemType, "message") != 0) continue;
        for (JsonObjectConst content : item["content"].as<JsonArrayConst>()) {
            const char* contentType = content["type"].as<const char*>();
            if (!contentType) contentType = "";
            if (strcmp(contentType, "output_text") != 0) continue;
            const char* text = content["text"].as<const char*>();
            if (!text || text[0] == '\0') continue;
            if (!appendText_(textOut, textOutLen, used, text)) {
                writeText_(errOut, errOutLen, "OpenAI output exceeds bounded capacity");
                textOut[0] = '\0';
                return false;
            }
        }
    }
    if (used == 0U) {
        char detail[192]{};
        snprintf(detail,
                 sizeof(detail),
                 "OpenAI completed response has no output text");
        writeText_(errOut, errOutLen, detail);
        return false;
    }
    return true;
}

}  // namespace OpenAiResponsesParser
