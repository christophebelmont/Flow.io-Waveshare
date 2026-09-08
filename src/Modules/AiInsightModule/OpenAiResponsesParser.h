#pragma once
/**
 * @file OpenAiResponsesParser.h
 * @brief Parses the bounded text result returned by the OpenAI Responses API.
 */

#include <ArduinoJson.h>
#include <stddef.h>

namespace OpenAiResponsesParser {

struct Result {
    char responseId[96]{};
    char model[48]{};
};

bool parse(JsonVariantConst root,
           Result& out,
           char* textOut,
           size_t textOutLen,
           char* errOut,
           size_t errOutLen);

bool extractApiError(JsonVariantConst root,
                     char* errOut,
                     size_t errOutLen);

}  // namespace OpenAiResponsesParser
