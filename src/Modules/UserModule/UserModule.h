#pragma once
/**
 * @file UserModule.h
 * @brief User accounts, credential hashing and session tokens.
 *
 * Ownership of identity for the firmware. The module:
 *  - provisions a default `admin` account with a generated password on first boot;
 *  - stores salted SHA-256 password hashes as raw NVS blobs (never in ConfigStore
 *    JSON, so credentials never leak through config export);
 *  - issues stateless, HMAC-SHA256 signed, time-boxed session tokens;
 *  - exposes account management through the `UserService`.
 */

#include "Core/Module.h"
#include "Core/NvsKeys.h"
#include "Core/ServiceBinding.h"
#include "Core/Services/Services.h"
#include "Core/Services/IUser.h"

class UserModule : public Module {
public:
    UserModule() = default;

    ModuleId moduleId() const override { return ModuleId::User; }
    const char* taskName() const override { return "users"; }
    uint8_t taskCount() const override { return 0; }

    uint8_t dependencyCount() const override { return 2; }
    ModuleId dependency(uint8_t i) const override {
        if (i == 0) return ModuleId::LogHub;
        if (i == 1) return ModuleId::ConfigStore;
        return ModuleId::Unknown;
    }

    void init(ConfigStore& cfg, ServiceRegistry& services) override;
    void onConfigLoaded(ConfigStore&, ServiceRegistry&) override;
    void loop() override {}

private:
    static constexpr uint8_t kMaxAccounts = 10U;
    static constexpr size_t kUsernameMax = 32U;
    static constexpr size_t kSaltLen = 16U;
    static constexpr size_t kHashLen = 32U;
    static constexpr size_t kSecretLen = 32U;
    static constexpr uint32_t kTokenTtlSeconds = 86400U; // 24 hours

    struct AccountRecord {
        char username[kUsernameMax];
        UserRole role;
        uint8_t reserved[3];
        uint32_t tokenEpoch;
        uint8_t salt[kSaltLen];
        uint8_t hash[kHashLen];
    };
    static_assert(sizeof(AccountRecord) == 88, "AccountRecord layout changed");

    struct SessionPayload {
        uint8_t version;
        uint8_t role;
        uint8_t reserved[2];
        uint32_t tokenEpoch;
        uint64_t issuedAt;
        uint64_t expiresAt;
        uint8_t usernameLen;
        char username[kUsernameMax];
    } __attribute__((packed));
    static_assert(sizeof(SessionPayload) == 57, "SessionPayload layout changed");

    ConfigStore* cfgStore_ = nullptr;
    ServiceRegistry* services_ = nullptr;

    uint8_t secret_[kSecretLen] = {0};
    bool secretValid_ = false;

    char initialAdminPassword_[33] = {0};
    bool initialAdminPasswordAvailable_ = false;

    UserService userSvc_{
        ServiceBinding::bind<&UserModule::authenticate_>,
        ServiceBinding::bind<&UserModule::authorize_>,
        ServiceBinding::bind<&UserModule::sessionInfo_>,
        ServiceBinding::bind<&UserModule::listUsers_>,
        ServiceBinding::bind<&UserModule::saveUser_>,
        ServiceBinding::bind<&UserModule::deleteUser_>,
        ServiceBinding::bind<&UserModule::changeOwnPassword_>,
        ServiceBinding::bind<&UserModule::getInitialCredentials_>,
        this
    };

    // Provisioning
    void ensureProvisioned_();
    void accountKey_(char* out, size_t outLen, uint8_t slot);
    uint8_t accountCount_();
    bool loadAccount_(uint8_t slot, AccountRecord* out);
    bool writeAccount_(uint8_t slot, const AccountRecord& record);
    bool eraseAccount_(uint8_t slot);
    int8_t findAccountSlot_(const char* username);
    int8_t findFreeSlot_();
    bool loadSecret_();
    bool persistSecret_();
    void generateInitialAdmin_();

    // Crypto
    static void sha256_(const uint8_t* data, size_t len, uint8_t out[kHashLen]);
    static void hmacSha256_(const uint8_t* key, size_t keyLen,
                            const uint8_t* data, size_t dataLen,
                            uint8_t out[kHashLen]);
    static bool constantTimeEquals_(const uint8_t* a, const uint8_t* b, size_t len);
    void hashPassword_(const uint8_t salt[kSaltLen], const char* password, uint8_t out[kHashLen]);
    bool verifyPassword_(const AccountRecord& record, const char* password);

    // Session tokens
    uint64_t nowEpoch_() const;
    bool issueToken_(const AccountRecord& record, char* out, size_t outLen);
    bool parseToken_(const char* token,
                     SessionPayload* payloadOut,
                     uint8_t hmacOut[kHashLen]) const;

    // Service implementation
    bool authenticate_(const char* username, const char* password,
                       char* tokenOut, size_t tokenOutLen,
                       char* errOut, size_t errOutLen);
    bool authorize_(const char* token, UserRole* outRole);
    bool sessionInfo_(const char* token, UserRole* outRole,
                      char* usernameOut, size_t usernameOutLen);
    bool validateToken_(const char* token, UserRole* outRole,
                        char* usernameOut, size_t usernameOutLen);
    bool listUsers_(const char* adminToken, char* out, size_t outLen, bool* truncated);
    bool saveUser_(const char* adminToken, const char* username, const char* password,
                   UserRole role, char* errOut, size_t errOutLen);
    bool deleteUser_(const char* adminToken, const char* username,
                     char* errOut, size_t errOutLen);
    bool changeOwnPassword_(const char* token, const char* newPassword,
                            char* errOut, size_t errOutLen);
    bool getInitialCredentials_(char* usernameOut, size_t usernameOutLen,
                                char* passwordOut, size_t passwordOutLen);
};
