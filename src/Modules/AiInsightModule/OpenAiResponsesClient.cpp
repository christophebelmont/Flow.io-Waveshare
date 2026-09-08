/**
 * @file OpenAiResponsesClient.cpp
 * @brief Calls the OpenAI Responses API and extracts its bounded text output.
 */

#include "Modules/AiInsightModule/OpenAiResponsesClient.h"

#include "Core/BoundedBufferStream.h"
#include "Core/LogModuleIds.h"

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::AiInsightModule)
#include "Core/ModuleLog.h"

#include <ArduinoJson.h>
#include <HTTPClient.h>
#include <NetworkClientSecure.h>
#include <esp_heap_caps.h>
#include <new>
#include <stdio.h>
#include <string.h>

namespace {

constexpr char kOpenAiResponsesUrl[] = "https://api.openai.com/v1/responses";
const char* kDiagnosticHeaders[] = {
    "x-request-id",
    "x-ratelimit-remaining-requests",
    "x-ratelimit-remaining-tokens",
    "x-ratelimit-reset-requests",
    "x-ratelimit-reset-tokens",
};

struct SpiRamJsonAllocator {
    void* allocate(size_t size)
    {
        return heap_caps_malloc(size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
    }

    void deallocate(void* pointer)
    {
        heap_caps_free(pointer);
    }
};

using SpiRamJsonDocument = BasicJsonDocument<SpiRamJsonAllocator>;

class SpiRamBuffer {
public:
    explicit SpiRamBuffer(size_t capacity)
        : capacity_(capacity),
          data_(static_cast<char*>(
              heap_caps_calloc(1U, capacity, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT)))
    {
    }

    ~SpiRamBuffer()
    {
        if (data_) heap_caps_free(data_);
    }

    SpiRamBuffer(const SpiRamBuffer&) = delete;
    SpiRamBuffer& operator=(const SpiRamBuffer&) = delete;

    char* data() const { return data_; }
    size_t capacity() const { return capacity_; }
    explicit operator bool() const { return data_ != nullptr; }

private:
    size_t capacity_ = 0U;
    char* data_ = nullptr;
};

bool writeError_(char* out, size_t outLen, const char* message)
{
    if (!out || outLen == 0U) return false;
    const int written = snprintf(out,
                                 outLen,
                                 "%s",
                                 message ? message : "OpenAI request failed");
    return written >= 0 && (size_t)written < outLen;
}

}  // namespace

bool OpenAiResponsesClient::generate(const char* apiKey,
                                     const char* model,
                                     const char* prompt,
                                     OpenAiResponsesParser::Result& resultOut,
                                     char* textOut,
                                     size_t textOutLen,
                                     char* errOut,
                                     size_t errOutLen) const
{
    resultOut = OpenAiResponsesParser::Result{};
    if (textOut && textOutLen > 0U) textOut[0] = '\0';
    if (errOut && errOutLen > 0U) errOut[0] = '\0';
    if (!apiKey || apiKey[0] == '\0') {
        writeError_(errOut, errOutLen, "OpenAI API key is not configured");
        return false;
    }
    if (!model || model[0] == '\0') {
        writeError_(errOut, errOutLen, "OpenAI model is not configured");
        return false;
    }
    if (!prompt || prompt[0] == '\0') {
        writeError_(errOut, errOutLen, "OpenAI prompt is empty");
        return false;
    }
    if (!textOut || textOutLen == 0U) {
        writeError_(errOut, errOutLen, "OpenAI result storage unavailable");
        return false;
    }

    SpiRamBuffer requestBuffer(kRequestCapacity);
    SpiRamBuffer responseBuffer(kResponseCapacity + 1U);
    if (!requestBuffer || !responseBuffer) {
        writeError_(errOut, errOutLen, "OpenAI PSRAM work buffers unavailable");
        return false;
    }

    SpiRamJsonDocument requestDocument(kRequestJsonCapacity);
    if (requestDocument.capacity() < kRequestJsonCapacity) {
        writeError_(errOut, errOutLen, "OpenAI request JSON storage unavailable");
        return false;
    }
    requestDocument["model"] = model;
    requestDocument["input"] = prompt;
    requestDocument["max_output_tokens"] = kMaxOutputTokens;
    requestDocument["store"] = false;
    const size_t requiredRequestBytes = measureJson(requestDocument) + 1U;
    if (requiredRequestBytes > requestBuffer.capacity()) {
        writeError_(errOut, errOutLen, "OpenAI request exceeds bounded capacity");
        return false;
    }
    const size_t requestBytes = serializeJson(requestDocument,
                                              requestBuffer.data(),
                                              requestBuffer.capacity());
    if (requestBytes == 0U || requestBytes >= requestBuffer.capacity()) {
        writeError_(errOut, errOutLen, "OpenAI request serialization failed");
        return false;
    }

    char authorization[224]{};
    const int authorizationLength = snprintf(authorization,
                                             sizeof(authorization),
                                             "Bearer %s",
                                             apiKey);
    if (authorizationLength <= 0 || (size_t)authorizationLength >= sizeof(authorization)) {
        writeError_(errOut, errOutLen, "OpenAI API key is too long");
        return false;
    }

    NetworkClientSecure client;
    client.setInsecure();
    HTTPClient http;
    http.setReuse(false);
    http.setConnectTimeout(kConnectTimeoutMs);
    http.setTimeout(kRequestTimeoutMs);
    if (!http.begin(client, kOpenAiResponsesUrl)) {
        writeError_(errOut, errOutLen, "OpenAI HTTP initialization failed");
        return false;
    }
    http.addHeader("Authorization", authorization);
    http.addHeader("Content-Type", "application/json");
    http.addHeader("Accept", "application/json");
    http.addHeader("Accept-Encoding", "identity");
    http.collectHeaders(kDiagnosticHeaders,
                        sizeof(kDiagnosticHeaders) / sizeof(kDiagnosticHeaders[0]));

    LOGI("OpenAI request started model=%s request_bytes=%u",
         model,
         (unsigned)requestBytes);
    const int statusCode = http.POST(reinterpret_cast<uint8_t*>(requestBuffer.data()),
                                     requestBytes);
    if (statusCode < 0) {
        char tlsErrorText[128]{};
        const int tlsError = client.lastError(tlsErrorText, sizeof(tlsErrorText));
        const String httpErrorText = HTTPClient::errorToString(statusCode);
        if (errOut && errOutLen > 0U) {
            snprintf(errOut,
                     errOutLen,
                     "OpenAI HTTPS failed: %s (%d), transport error %d",
                     httpErrorText.c_str(),
                     statusCode,
                     tlsError);
        }
        LOGE("OpenAI HTTPS failed http=%d (%s) transport=%d (%s)",
             statusCode,
             httpErrorText.c_str(),
             tlsError,
             tlsErrorText[0] ? tlsErrorText : "no transport detail");
        http.end();
        return false;
    }

    const int announcedSize = http.getSize();
    char requestId[80]{};
    char remainingRequests[24]{};
    char remainingTokens[24]{};
    char resetRequests[32]{};
    char resetTokens[32]{};
    snprintf(requestId, sizeof(requestId), "%s", http.header(kDiagnosticHeaders[0]).c_str());
    snprintf(remainingRequests,
             sizeof(remainingRequests),
             "%s",
             http.header(kDiagnosticHeaders[1]).c_str());
    snprintf(remainingTokens,
             sizeof(remainingTokens),
             "%s",
             http.header(kDiagnosticHeaders[2]).c_str());
    snprintf(resetRequests,
             sizeof(resetRequests),
             "%s",
             http.header(kDiagnosticHeaders[3]).c_str());
    snprintf(resetTokens,
             sizeof(resetTokens),
             "%s",
             http.header(kDiagnosticHeaders[4]).c_str());
    if (announcedSize == 0) {
        writeError_(errOut, errOutLen, "OpenAI response is empty");
        http.end();
        return false;
    }
    if (announcedSize > 0 && (size_t)announcedSize > kResponseCapacity) {
        writeError_(errOut, errOutLen, "OpenAI response is too large");
        http.end();
        return false;
    }

    BoundedBufferStream sink(responseBuffer.data(), kResponseCapacity);
    const int written = http.writeToStream(&sink);
    const size_t responseBytes = sink.length();
    const bool overflowed = sink.overflowed();
    http.end();
    if (overflowed) {
        writeError_(errOut, errOutLen, "OpenAI response is too large");
        return false;
    }
    if (written < 0 || responseBytes == 0U ||
        (announcedSize > 0 && responseBytes != (size_t)announcedSize)) {
        writeError_(errOut, errOutLen, "OpenAI response read failed");
        return false;
    }
    responseBuffer.data()[responseBytes] = '\0';

    SpiRamJsonDocument responseDocument(kResponseJsonCapacity);
    if (responseDocument.capacity() < kResponseJsonCapacity) {
        writeError_(errOut, errOutLen, "OpenAI response JSON storage unavailable");
        return false;
    }
    const DeserializationError jsonError =
        deserializeJson(responseDocument, responseBuffer.data(), responseBytes);
    if (jsonError) {
        writeError_(errOut, errOutLen, "OpenAI response contains invalid JSON");
        LOGE("OpenAI response JSON invalid status=%d bytes=%u detail=%s",
             statusCode,
             (unsigned)responseBytes,
             jsonError.c_str());
        return false;
    }

    const JsonVariantConst root = responseDocument.as<JsonVariantConst>();
    if (statusCode != HTTP_CODE_OK) {
        if (!OpenAiResponsesParser::extractApiError(root, errOut, errOutLen)) {
            if (errOut && errOutLen > 0U) {
                snprintf(errOut, errOutLen, "OpenAI HTTP status %d", statusCode);
            }
        }
        LOGW("OpenAI API rejected request status=%d bytes=%u request_id=%s "
             "remaining_requests=%s remaining_tokens=%s reset_requests=%s "
             "reset_tokens=%s detail=%s",
             statusCode,
             (unsigned)responseBytes,
             requestId[0] ? requestId : "-",
             remainingRequests[0] ? remainingRequests : "-",
             remainingTokens[0] ? remainingTokens : "-",
             resetRequests[0] ? resetRequests : "-",
             resetTokens[0] ? resetTokens : "-",
             errOut && errOut[0] ? errOut : "unknown");
        return false;
    }

    if (!OpenAiResponsesParser::parse(root,
                                      resultOut,
                                      textOut,
                                      textOutLen,
                                      errOut,
                                      errOutLen)) {
        const uint32_t outputTokens = root["usage"]["output_tokens"] | 0U;
        const uint32_t reasoningTokens =
            root["usage"]["output_tokens_details"]["reasoning_tokens"] | 0U;
        LOGW("OpenAI response parse failed bytes=%u output_tokens=%lu "
             "reasoning_tokens=%lu max_output_tokens=%u detail=%s",
             (unsigned)responseBytes,
             (unsigned long)outputTokens,
             (unsigned long)reasoningTokens,
             (unsigned)kMaxOutputTokens,
             errOut && errOut[0] ? errOut : "unknown");
        return false;
    }
    LOGI("OpenAI response ready id=%s model=%s response_bytes=%u text_bytes=%u",
         resultOut.responseId[0] ? resultOut.responseId : "-",
         resultOut.model[0] ? resultOut.model : model,
         (unsigned)responseBytes,
         (unsigned)strlen(textOut));
    return true;
}
