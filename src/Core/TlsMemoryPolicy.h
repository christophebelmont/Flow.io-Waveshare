#pragma once
/**
 * @file TlsMemoryPolicy.h
 * @brief Installs the process-wide memory policy used by mbedTLS.
 */

namespace TlsMemoryPolicy {

/**
 * @brief Makes mbedTLS allocate from PSRAM first, with internal RAM fallback.
 *
 * This must be called once during platform bootstrap, before network modules
 * can create TLS contexts.
 *
 * @return true when the policy was installed, false when PSRAM is unavailable
 *         or mbedTLS rejected the allocator callbacks.
 */
bool installPsramPreferred();

}  // namespace TlsMemoryPolicy
