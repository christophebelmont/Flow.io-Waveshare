#pragma once
/**
 * @file IUser.h
 * @brief User identity, roles, permissions and account management service.
 *
 * The service contract follows the project convention of C function pointers
 * with a `void* ctx`. Authorization for account management is enforced inside
 * the service: callers provide their session token, and the service resolves
 * the role before mutating any account.
 */

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/** @brief Account role, ordered by increasing privilege. */
enum class UserRole : uint8_t {
    None = 0,
    Operator = 1,
    Admin = 2
};

/** @brief Coarse permission capabilities (kept minimal for the first release). */
enum class UserPermission : uint8_t {
    ReadState = 1u << 0,    ///< Read dashboard/runtime/telemetry.
    ControlPool = 1u << 1,  ///< Pilot equipment and pool configuration.
    UpdateSystem = 1u << 2, ///< Firmware/SPIFFS/Nextion updates and system actions.
    ManageUsers = 1u << 3   ///< Create/modify/delete accounts.
};

/** @brief Resolve whether a role holds a given permission (single source of truth). */
static inline bool roleHasPermission(UserRole role, UserPermission permission)
{
    switch (role) {
        case UserRole::Admin:
            return true;
        case UserRole::Operator:
            return permission == UserPermission::ReadState ||
                   permission == UserPermission::ControlPool;
        case UserRole::None:
        default:
            return false;
    }
}

/** @brief Stable text name for a role. */
static inline const char* userRoleName(UserRole role)
{
    switch (role) {
        case UserRole::Admin: return "admin";
        case UserRole::Operator: return "operator";
        case UserRole::None:
        default: return "none";
    }
}

/** @brief Parse a role name into a UserRole. Returns false on unknown input. */
static inline bool userRoleFromName(const char* name, UserRole* out)
{
    if (!name || !out) return false;
    if (strcmp(name, "admin") == 0 || strcmp(name, "Admin") == 0) {
        *out = UserRole::Admin;
        return true;
    }
    if (strcmp(name, "operator") == 0 || strcmp(name, "Operator") == 0) {
        *out = UserRole::Operator;
        return true;
    }
    return false;
}

/** @brief Service interface for user identity and account management. */
struct UserService {
    /** @brief Validate credentials and issue a session token. */
    bool (*authenticate)(void* ctx,
                         const char* username,
                         const char* password,
                         char* tokenOut,
                         size_t tokenOutLen,
                         char* errOut,
                         size_t errOutLen);
    /** @brief Validate a session token and return the resolved role. */
    bool (*authorize)(void* ctx, const char* token, UserRole* outRole);
    /** @brief Validate a session token and return role plus the username. */
    bool (*sessionInfo)(void* ctx,
                        const char* token,
                        UserRole* outRole,
                        char* usernameOut,
                        size_t usernameOutLen);
    /** @brief List accounts as JSON (admin only). Never exposes credentials. */
    bool (*listUsers)(void* ctx,
                      const char* adminToken,
                      char* out,
                      size_t outLen,
                      bool* truncated);
    /** @brief Create or update an account (admin only). Empty password keeps existing. */
    bool (*saveUser)(void* ctx,
                     const char* adminToken,
                     const char* username,
                     const char* password,
                     UserRole role,
                     char* errOut,
                     size_t errOutLen);
    /** @brief Delete an account (admin only). */
    bool (*deleteUser)(void* ctx,
                       const char* adminToken,
                       const char* username,
                       char* errOut,
                       size_t errOutLen);
    /** @brief Change the password of the account identified by the token. */
    bool (*changeOwnPassword)(void* ctx,
                              const char* token,
                              const char* newPassword,
                              char* errOut,
                              size_t errOutLen);
    /** @brief Read one-time initial admin credentials (cleared after first login). */
    bool (*getInitialCredentials)(void* ctx,
                                  char* usernameOut,
                                  size_t usernameOutLen,
                                  char* passwordOut,
                                  size_t passwordOutLen);
    void* ctx;
};
