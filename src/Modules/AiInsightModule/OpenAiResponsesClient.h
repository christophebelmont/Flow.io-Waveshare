#pragma once
/**
 * @file OpenAiResponsesClient.h
 * @brief Bounded HTTPS client for the OpenAI Responses API.
 */

#include "Modules/AiInsightModule/OpenAiResponsesParser.h"

#include <stddef.h>

class OpenAiResponsesClient {
public:
    bool generate(const char* apiKey,
                  const char* model,
                  const char* instructions,
                  const char* input,
                  OpenAiResponsesParser::Result& resultOut,
                  char* textOut,
                  size_t textOutLen,
                  char* errOut,
                  size_t errOutLen) const;

private:
    static constexpr size_t kRequestCapacity = 40U * 1024U;
    static constexpr size_t kResponseCapacity = 24U * 1024U;
    static constexpr size_t kRequestJsonCapacity = 32U * 1024U;
    static constexpr size_t kResponseJsonCapacity = 32U * 1024U;
    static constexpr uint32_t kConnectTimeoutMs = 10000U;
    static constexpr uint32_t kRequestTimeoutMs = 45000U;
    // The Responses API counts both visible text and internal reasoning tokens
    // against this limit. Keep enough headroom for reasoning models while the
    // prompt itself constrains the visible answer to three short paragraphs.
    static constexpr uint16_t kMaxOutputTokens = 2048U;
};
