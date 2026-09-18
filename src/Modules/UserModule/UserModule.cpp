/**
 * @file UserModule.cpp
 * @brief Implementation of user accounts, credential hashing and session tokens.
 */
#include "UserModule.h"

#include "Core/ErrorCodes.h"

#include <string.h>
#include <ctype.h>
#include <stdio.h>

#include <mbedtls/sha256.h>
#include <esp_random.h>

#define LOG_MODULE_ID ((LogModuleId)LogModuleIdValue::UserModule)
#include "Core/ModuleLog.h"

namespace {

constexpr size_t kSessionPayloadBytes = 57U;
constexpr size_t kHashHexLen = 64U;    // 32 bytes -> 64 hex chars
constexpr size_t kPayloadHexLen = 114U; // 57 bytes -> 114 hex chars
constexpr size_t kTokenHexLen = kPayloadHexLen + kHashHexLen; // 178 hex chars

constexpr uint8_t kSessionVersion = 1U;

const char kHexDigits[] = "0123456789abcdef";

// Well-known first-boot administrator credential. The owner is expected to
// change this password from the web "Comptes" page after initial setup.
constexpr char kDefaultAdminUsername[] = "admin";
constexpr char kDefaultAdminPassword[] = "flowio1234";

uint8_t hexNibble_(char c)
{
    if (c >= '0' && c <= '9') return (uint8_t)(c - '0');
    if (c >= 'a' && c <= 'f') return (uint8_t)(c - 'a' + 10);
    if (c >= 'A' && c <= 'F') return (uint8_t)(c - 'A' + 10);
    return 0xFF;
}

size_t hexEncode_(const uint8_t* in, size_t len, char* out, size_t outCap)
{
    if (!in || !out || outCap < len * 2U + 1U) return 0U;
    for (size_t i = 0; i < len; ++i) {
        out[i * 2U] = kHexDigits[in[i] >> 4];
        out[i * 2U + 1U] = kHexDigits[in[i] & 0x0Fu];
    }
    out[len * 2U] = '\0';
    return len * 2U;
}

bool hexDecode_(const char* in, size_t hexLen, uint8_t* out, size_t outCap)
{
    if (!in || !out || (hexLen & 1U) != 0U) return false;
    const size_t n = hexLen / 2U;
    if (n > outCap) return false;
    for (size_t i = 0; i < n; ++i) {
        const uint8_t hi = hexNibble_(in[i * 2U]);
        const uint8_t lo = hexNibble_(in[i * 2U + 1U]);
        if (hi > 15U || lo > 15U) return false;
        out[i] = (uint8_t)((hi << 4) | lo);
    }
    return true;
}

bool isBlankText_(const char* text, size_t maxLen)
{
    if (!text) return true;
    const size_t len = strnlen(text, maxLen);
    if (len == 0U || len >= maxLen) return true;
    for (size_t i = 0; i < len; ++i) {
        if (!isspace((unsigned char)text[i])) return false;
    }
    return true;
}

bool isValidUsername_(const char* username, size_t maxLen)
{
    if (isBlankText_(username, maxLen)) return false;
    const size_t len = strnlen(username, maxLen);
    if (len == 0U || len >= maxLen) return false;
    for (size_t i = 0; i < len; ++i) {
        const char c = username[i];
        if (!isalnum((unsigned char)c) && c != '-' && c != '_' && c != '.') {
            return false;
        }
    }
    return true;
}

}  // namespace

void UserModule::init(ConfigStore& cfg, ServiceRegistry& services)
{
    cfgStore_ = &cfg;
    services_ = &services;

    if (!services.add(ServiceId::User, &userSvc_)) {
        LOGE("service registration failed: %s", toString(ServiceId::User));
    }
    LOGI("User identity service registered");
}

void UserModule::onConfigLoaded(ConfigStore&, ServiceRegistry&)
{
    ensureProvisioned_();
}

void UserModule::ensureProvisioned_()
{
    if (!cfgStore_) return;

    if (!loadSecret_()) {
        uint8_t generated[kSecretLen] = {0};
        esp_fill_random(generated, kSecretLen);
        memcpy(secret_, generated, kSecretLen);
        secretValid_ = persistSecret_();
        if (!secretValid_) {
            LOGE("Failed to persist session secret");
        }
    }

    if (accountCount_() == 0U) {
        generateInitialAdmin_();
    }
}

uint8_t UserModule::accountCount_()
{
    uint8_t count = 0;
    for (uint8_t slot = 0; slot < kMaxAccounts; ++slot) {
        AccountRecord record{};
        if (loadAccount_(slot, &record) && record.username[0] != '\0') {
            ++count;
        }
    }
    return count;
}

void UserModule::accountKey_(char* out, size_t outLen, uint8_t slot)
{
    if (out && outLen > 0U) {
        snprintf(out, outLen, "%s%u", NvsKeys::Users::AccountKeyPrefix, (unsigned)slot);
    }
}

bool UserModule::loadAccount_(uint8_t slot, AccountRecord* out)
{
    if (!cfgStore_ || slot >= kMaxAccounts || !out) return false;
    char key[16] = {0};
    accountKey_(key, sizeof(key), slot);
    size_t actualLen = 0U;
    if (!cfgStore_->readRuntimeBlob(key, out, sizeof(AccountRecord), &actualLen)) {
        return false;
    }
    if (actualLen != sizeof(AccountRecord)) {
        return false;
    }
    return out->username[0] != '\0';
}

bool UserModule::writeAccount_(uint8_t slot, const AccountRecord& record)
{
    if (!cfgStore_ || slot >= kMaxAccounts) return false;
    char key[16] = {0};
    accountKey_(key, sizeof(key), slot);
    return cfgStore_->writeRuntimeBlob(key, &record, sizeof(AccountRecord));
}

bool UserModule::eraseAccount_(uint8_t slot)
{
    if (!cfgStore_ || slot >= kMaxAccounts) return false;
    char key[16] = {0};
    accountKey_(key, sizeof(key), slot);
    return cfgStore_->eraseKey(key);
}

int8_t UserModule::findAccountSlot_(const char* username)
{
    if (!username) return -1;
    for (uint8_t slot = 0; slot < kMaxAccounts; ++slot) {
        AccountRecord record{};
        if (loadAccount_(slot, &record) && strncmp(record.username, username, kUsernameMax) == 0) {
            return (int8_t)slot;
        }
    }
    return -1;
}

int8_t UserModule::findFreeSlot_()
{
    for (uint8_t slot = 0; slot < kMaxAccounts; ++slot) {
        AccountRecord record{};
        if (!loadAccount_(slot, &record)) {
            return (int8_t)slot;
        }
    }
    return -1;
}

bool UserModule::loadSecret_()
{
    if (!cfgStore_) return false;
    size_t actualLen = 0U;
    if (!cfgStore_->readRuntimeBlob(NvsKeys::Users::SessionSecret, secret_, kSecretLen, &actualLen)) {
        return false;
    }
    secretValid_ = (actualLen == kSecretLen);
    return secretValid_;
}

bool UserModule::persistSecret_()
{
    if (!cfgStore_) return false;
    return cfgStore_->writeRuntimeBlob(NvsKeys::Users::SessionSecret, secret_, kSecretLen);
}

void UserModule::generateInitialAdmin_()
{
    AccountRecord admin{};
    snprintf(admin.username, sizeof(admin.username), "%s", kDefaultAdminUsername);
    admin.role = UserRole::Admin;
    admin.tokenEpoch = 1U;
    esp_fill_random(admin.salt, kSaltLen);
    hashPassword_(admin.salt, kDefaultAdminPassword, admin.hash);

    const int8_t slot = findFreeSlot_();
    if (slot < 0) {
        LOGE("No free account slot for initial admin");
        return;
    }
    if (!writeAccount_((uint8_t)slot, admin)) {
        LOGE("Failed to persist initial admin account");
        return;
    }

    snprintf(initialAdminPassword_, sizeof(initialAdminPassword_), "%s", kDefaultAdminPassword);
    initialAdminPasswordAvailable_ = true;

    LOGW("Default admin account provisioned: username=%s password=%s (change it after first login)",
         kDefaultAdminUsername, kDefaultAdminPassword);
}

void UserModule::sha256_(const uint8_t* data, size_t len, uint8_t out[kHashLen])
{
    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, data, len);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

void UserModule::hmacSha256_(const uint8_t* key,
                             size_t keyLen,
                             const uint8_t* data,
                             size_t dataLen,
                             uint8_t out[kHashLen])
{
    uint8_t k[64] = {0};
    if (keyLen > 64U) {
        sha256_(key, keyLen, k);
    } else {
        memcpy(k, key, keyLen);
    }

    uint8_t ipad[64];
    uint8_t opad[64];
    for (size_t i = 0; i < 64U; ++i) {
        ipad[i] = k[i] ^ 0x36;
        opad[i] = k[i] ^ 0x5c;
    }

    uint8_t inner[kHashLen] = {0};

    mbedtls_sha256_context ctx;
    mbedtls_sha256_init(&ctx);
    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, ipad, 64U);
    mbedtls_sha256_update(&ctx, data, dataLen);
    mbedtls_sha256_finish(&ctx, inner);

    mbedtls_sha256_starts(&ctx, 0);
    mbedtls_sha256_update(&ctx, opad, 64U);
    mbedtls_sha256_update(&ctx, inner, kHashLen);
    mbedtls_sha256_finish(&ctx, out);
    mbedtls_sha256_free(&ctx);
}

bool UserModule::constantTimeEquals_(const uint8_t* a, const uint8_t* b, size_t len)
{
    if (!a || !b) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < len; ++i) {
        diff |= (a[i] ^ b[i]);
    }
    return diff == 0U;
}

void UserModule::hashPassword_(const uint8_t salt[kSaltLen],
                               const char* password,
                               uint8_t out[kHashLen])
{
    uint8_t buffer[kSaltLen + 96] = {0};
    const size_t pwLen = password ? strnlen(password, 95U) : 0U;
    memcpy(buffer, salt, kSaltLen);
    if (pwLen > 0U) {
        memcpy(buffer + kSaltLen, password, pwLen);
    }
    sha256_(buffer, kSaltLen + pwLen, out);
}

bool UserModule::verifyPassword_(const AccountRecord& record, const char* password)
{
    uint8_t computed[kHashLen] = {0};
    hashPassword_(record.salt, password, computed);
    return constantTimeEquals_(computed, record.hash, kHashLen);
}

uint64_t UserModule::nowEpoch_() const
{
    if (!services_) return 0U;
    const TimeService* time = services_->get<TimeService>(ServiceId::Time);
    if (!time) return 0U;
    if (time->currentState) {
        TimeState state{};
        if (time->currentState(time->ctx, &state) && state.valid) {
            return state.currentTimeUtc;
        }
    }
    if (time->epoch) {
        return time->epoch(time->ctx);
    }
    return 0U;
}

bool UserModule::issueToken_(const AccountRecord& record, char* out, size_t outLen)
{
    if (!out || outLen == 0U) return false;
    out[0] = '\0';
    if (!secretValid_) return false;

    const uint64_t now = nowEpoch_();
    if (now == 0U) return false;

    SessionPayload payload{};
    payload.version = kSessionVersion;
    payload.role = (uint8_t)record.role;
    payload.tokenEpoch = record.tokenEpoch;
    payload.issuedAt = now;
    payload.expiresAt = now + kTokenTtlSeconds;
    payload.usernameLen = (uint8_t)strnlen(record.username, kUsernameMax);
    memcpy(payload.username, record.username, payload.usernameLen);

    uint8_t mac[kHashLen] = {0};
    hmacSha256_(secret_, kSecretLen,
                reinterpret_cast<const uint8_t*>(&payload), kSessionPayloadBytes,
                mac);

    char payloadHex[kPayloadHexLen + 1U] = {0};
    char macHex[kHashHexLen + 1U] = {0};
    hexEncode_(reinterpret_cast<const uint8_t*>(&payload), kSessionPayloadBytes,
               payloadHex, sizeof(payloadHex));
    hexEncode_(mac, kHashLen, macHex, sizeof(macHex));

    const int wrote = snprintf(out, outLen, "%s%s", payloadHex, macHex);
    return wrote > 0 && (size_t)wrote < outLen;
}

bool UserModule::parseToken_(const char* token,
                             SessionPayload* payloadOut,
                             uint8_t hmacOut[kHashLen]) const
{
    if (!token || !payloadOut || !hmacOut) return false;
    const size_t tokenLen = strlen(token);
    if (tokenLen != kTokenHexLen) return false;

    uint8_t payloadBytes[kSessionPayloadBytes] = {0};
    uint8_t macBytes[kHashLen] = {0};

    char payloadHex[kPayloadHexLen + 1U] = {0};
    char macHex[kHashHexLen + 1U] = {0};
    memcpy(payloadHex, token, kPayloadHexLen);
    payloadHex[kPayloadHexLen] = '\0';
    memcpy(macHex, token + kPayloadHexLen, kHashHexLen);
    macHex[kHashHexLen] = '\0';

    if (!hexDecode_(payloadHex, kPayloadHexLen, payloadBytes, sizeof(payloadBytes))) return false;
    if (!hexDecode_(macHex, kHashHexLen, macBytes, sizeof(macBytes))) return false;

    memcpy(payloadOut, payloadBytes, kSessionPayloadBytes);
    memcpy(hmacOut, macBytes, kHashLen);
    return true;
}

bool UserModule::authenticate_(const char* username,
                               const char* password,
                               char* tokenOut,
                               size_t tokenOutLen,
                               char* errOut,
                               size_t errOutLen)
{
    if (errOut && errOutLen > 0U) errOut[0] = '\0';
    if (!username || !password) {
        snprintf(errOut, errOutLen, "invalid_credentials");
        return false;
    }

    const int8_t slot = findAccountSlot_(username);
    if (slot < 0) {
        snprintf(errOut, errOutLen, "invalid_credentials");
        return false;
    }

    AccountRecord record{};
    if (!loadAccount_((uint8_t)slot, &record)) {
        snprintf(errOut, errOutLen, "unavailable");
        return false;
    }

    if (!verifyPassword_(record, password)) {
        LOGW("Authentication failed for username='%s'", username);
        snprintf(errOut, errOutLen, "invalid_credentials");
        return false;
    }

    if (!issueToken_(record, tokenOut, tokenOutLen)) {
        snprintf(errOut, errOutLen, "time_unavailable");
        return false;
    }

    // One-time initial credentials are consumed on first successful login.
    if (initialAdminPasswordAvailable_) {
        initialAdminPassword_[0] = '\0';
        initialAdminPasswordAvailable_ = false;
    }

    LOGI("Authentication success username='%s' role=%s",
         username, userRoleName(record.role));
    return true;
}

bool UserModule::authorize_(const char* token, UserRole* outRole)
{
    return validateToken_(token, outRole, nullptr, 0);
}

bool UserModule::sessionInfo_(const char* token, UserRole* outRole,
                              char* usernameOut, size_t usernameOutLen)
{
    return validateToken_(token, outRole, usernameOut, usernameOutLen);
}

bool UserModule::validateToken_(const char* token, UserRole* outRole,
                                char* usernameOut, size_t usernameOutLen)
{
    if (outRole) *outRole = UserRole::None;
    if (!token || token[0] == '\0') return false;
    if (!secretValid_) return false;

    SessionPayload payload{};
    uint8_t mac[kHashLen] = {0};
    if (!parseToken_(token, &payload, mac)) return false;
    if (payload.version != kSessionVersion) return false;

    uint8_t expected[kHashLen] = {0};
    hmacSha256_(secret_, kSecretLen,
                reinterpret_cast<const uint8_t*>(&payload), kSessionPayloadBytes,
                expected);
    if (!constantTimeEquals_(expected, mac, kHashLen)) return false;

    const uint64_t now = nowEpoch_();
    if (now == 0U) return false;
    if (now < payload.issuedAt || now >= payload.expiresAt) return false;

    char username[kUsernameMax] = {0};
    if (payload.usernameLen >= kUsernameMax) return false;
    memcpy(username, payload.username, payload.usernameLen);
    username[payload.usernameLen] = '\0';

    const int8_t slot = findAccountSlot_(username);
    if (slot < 0) return false;

    AccountRecord record{};
    if (!loadAccount_((uint8_t)slot, &record)) return false;
    if (record.tokenEpoch != payload.tokenEpoch) return false;

    if (outRole) *outRole = record.role;
    if (usernameOut && usernameOutLen > 0U) {
        snprintf(usernameOut, usernameOutLen, "%s", record.username);
    }
    return true;
}

bool UserModule::listUsers_(const char* adminToken, char* out, size_t outLen, bool* truncated)
{
    if (truncated) *truncated = false;
    if (!out || outLen == 0U) return false;

    UserRole adminRole = UserRole::None;
    if (!authorize_(adminToken, &adminRole) || !roleHasPermission(adminRole, UserPermission::ManageUsers)) {
        snprintf(out, outLen, "{\"ok\":false,\"err\":{\"code\":\"Forbidden\"}}");
        return false;
    }

    size_t pos = 0U;
    const auto write = [&](const char* s) {
        if (!s) return;
        const size_t n = strlen(s);
        if (pos + n + 1U >= outLen) {
            if (truncated) *truncated = true;
            return;
        }
        memcpy(out + pos, s, n);
        pos += n;
        out[pos] = '\0';
    };

    write("{\"ok\":true,\"accounts\":[");
    bool first = true;
    for (uint8_t slot = 0; slot < kMaxAccounts; ++slot) {
        AccountRecord record{};
        if (!loadAccount_(slot, &record)) continue;
        if (!first) write(",");
        first = false;

        char entry[96] = {0};
        snprintf(entry, sizeof(entry), "{\"username\":\"%s\",\"role\":\"%s\"}",
                 record.username, userRoleName(record.role));
        write(entry);
    }
    write("]}");
    return true;
}

bool UserModule::saveUser_(const char* adminToken,
                           const char* username,
                           const char* password,
                           UserRole role,
                           char* errOut,
                           size_t errOutLen)
{
    if (errOut && errOutLen > 0U) errOut[0] = '\0';

    UserRole adminRole = UserRole::None;
    if (!authorize_(adminToken, &adminRole) || !roleHasPermission(adminRole, UserPermission::ManageUsers)) {
        snprintf(errOut, errOutLen, "forbidden");
        return false;
    }
    if (!isValidUsername_(username, kUsernameMax)) {
        snprintf(errOut, errOutLen, "invalid_username");
        return false;
    }
    if (role != UserRole::Operator && role != UserRole::Admin) {
        snprintf(errOut, errOutLen, "invalid_role");
        return false;
    }
    const bool hasPassword = !isBlankText_(password, 96U);
    if (!hasPassword && findAccountSlot_(username) < 0) {
        snprintf(errOut, errOutLen, "missing_password");
        return false;
    }

    int8_t slot = findAccountSlot_(username);
    if (slot < 0) {
        slot = findFreeSlot_();
        if (slot < 0) {
            snprintf(errOut, errOutLen, "account_limit");
            return false;
        }
    }

    AccountRecord record{};
    if (!loadAccount_((uint8_t)slot, &record)) {
        // New account: initialize fresh fields.
        memset(&record, 0, sizeof(record));
        snprintf(record.username, sizeof(record.username), "%s", username);
        record.role = role;
        record.tokenEpoch = 0U;
    } else {
        record.role = role;
    }

    if (hasPassword) {
        esp_fill_random(record.salt, kSaltLen);
        hashPassword_(record.salt, password, record.hash);
        ++record.tokenEpoch;
    }

    if (record.tokenEpoch == 0U) {
        record.tokenEpoch = 1U;
    }

    if (!writeAccount_((uint8_t)slot, record)) {
        snprintf(errOut, errOutLen, "persist_failed");
        return false;
    }

    LOGI("Account saved username='%s' role=%s", username, userRoleName(role));
    return true;
}

bool UserModule::deleteUser_(const char* adminToken,
                             const char* username,
                             char* errOut,
                             size_t errOutLen)
{
    if (errOut && errOutLen > 0U) errOut[0] = '\0';

    UserRole adminRole = UserRole::None;
    if (!authorize_(adminToken, &adminRole) || !roleHasPermission(adminRole, UserPermission::ManageUsers)) {
        snprintf(errOut, errOutLen, "forbidden");
        return false;
    }
    if (!username || username[0] == '\0') {
        snprintf(errOut, errOutLen, "invalid_username");
        return false;
    }

    const int8_t slot = findAccountSlot_(username);
    if (slot < 0) {
        snprintf(errOut, errOutLen, "not_found");
        return false;
    }

    // Refuse to delete the last admin account.
    AccountRecord target{};
    loadAccount_((uint8_t)slot, &target);
    if (target.role == UserRole::Admin) {
        uint8_t adminCount = 0U;
        for (uint8_t s = 0; s < kMaxAccounts; ++s) {
            AccountRecord r{};
            if (loadAccount_(s, &r) && r.role == UserRole::Admin) ++adminCount;
        }
        if (adminCount <= 1U) {
            snprintf(errOut, errOutLen, "last_admin");
            return false;
        }
    }

    if (!eraseAccount_((uint8_t)slot)) {
        snprintf(errOut, errOutLen, "persist_failed");
        return false;
    }

    LOGI("Account deleted username='%s'", username);
    return true;
}

bool UserModule::changeOwnPassword_(const char* token,
                                    const char* newPassword,
                                    char* errOut,
                                    size_t errOutLen)
{
    if (errOut && errOutLen > 0U) errOut[0] = '\0';
    if (isBlankText_(newPassword, 96U)) {
        snprintf(errOut, errOutLen, "invalid_password");
        return false;
    }

    SessionPayload payload{};
    uint8_t mac[kHashLen] = {0};
    if (!authorize_(token, nullptr)) {
        snprintf(errOut, errOutLen, "unauthorized");
        return false;
    }
    if (!parseToken_(token, &payload, mac)) {
        snprintf(errOut, errOutLen, "unauthorized");
        return false;
    }

    char username[kUsernameMax] = {0};
    if (payload.usernameLen >= kUsernameMax) {
        snprintf(errOut, errOutLen, "unauthorized");
        return false;
    }
    memcpy(username, payload.username, payload.usernameLen);
    username[payload.usernameLen] = '\0';

    const int8_t slot = findAccountSlot_(username);
    if (slot < 0) {
        snprintf(errOut, errOutLen, "not_found");
        return false;
    }

    AccountRecord record{};
    if (!loadAccount_((uint8_t)slot, &record)) {
        snprintf(errOut, errOutLen, "unavailable");
        return false;
    }

    esp_fill_random(record.salt, kSaltLen);
    hashPassword_(record.salt, newPassword, record.hash);
    ++record.tokenEpoch;

    if (!writeAccount_((uint8_t)slot, record)) {
        snprintf(errOut, errOutLen, "persist_failed");
        return false;
    }

    LOGI("Password changed for username='%s'", username);
    return true;
}

bool UserModule::getInitialCredentials_(char* usernameOut,
                                        size_t usernameOutLen,
                                        char* passwordOut,
                                        size_t passwordOutLen)
{
    if (!initialAdminPasswordAvailable_ || initialAdminPassword_[0] == '\0') {
        return false;
    }
    if (usernameOut && usernameOutLen > 0U) {
        snprintf(usernameOut, usernameOutLen, "%s", kDefaultAdminUsername);
    }
    if (passwordOut && passwordOutLen > 0U) {
        snprintf(passwordOut, passwordOutLen, "%s", initialAdminPassword_);
    }
    return true;
}
