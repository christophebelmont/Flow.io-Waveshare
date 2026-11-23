#pragma once
/**
 * @file OtaHttpRetryPolicy.h
 * @brief Bounded HTTP retry and content-range validation for OTA artifacts.
 *
 * Header-only and independent of Arduino for host-side regression tests.
 * 3 attempts total, including the initial request (not 3 extra retries).
 */
#include <stddef.h>
#include <stdio.h>
#include <string.h>

namespace OtaHttpRetryPolicy {

constexpr unsigned kMaxAttempts = 3U;
constexpr unsigned kRetryDelayMs = 500U;

// Negative values originate from HTTPClient/TCP. 408/429/5xx are transient.
// Other positive responses are treated as permanent errors.
inline bool isRetryableStatus(int status) {
    return status < 0 || status == 408 || status == 429 ||
           (status >= 500 && status <= 599);
}

inline bool validContentRange(const char* header, size_t offset, size_t total) {
    if (!header || strncmp(header, "bytes ", 6U) != 0 ||
        offset == 0U || total == 0U || offset >= total) return false;
    unsigned long first = 0U, last = 0U, count = 0U;
    char trailing = '\0';
    if (sscanf(header, "bytes %lu-%lu/%lu %c", &first, &last, &count, &trailing) != 3)
        return false;
    return first == offset && last == total - 1U && count == total;
}

enum class ResponseMode : unsigned char {
    Invalid = 0,
    Full = 1,     // Server ignored Range or fresh GET: consume from byte zero.
    Partial = 2   // Validated 206 with exact Content-Range.
};

inline ResponseMode responseMode(int status, size_t requestedOffset,
                                 size_t contentLength, size_t expectedTotal,
                                 const char* contentRange) {
    if (expectedTotal == 0U) return ResponseMode::Invalid;
    if (status == 200 && contentLength == expectedTotal) return ResponseMode::Full;
    if (status == 206 && requestedOffset > 0U &&
        requestedOffset < expectedTotal &&
        contentLength == expectedTotal - requestedOffset &&
        validContentRange(contentRange, requestedOffset, expectedTotal))
        return ResponseMode::Partial;
    return ResponseMode::Invalid;
}

} // namespace OtaHttpRetryPolicy
