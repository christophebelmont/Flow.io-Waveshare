#pragma once
/**
 * @file Actor.h
 * @brief Identity responsible for an action, shared by commands and activity.
 */

#include <stddef.h>
#include <stdint.h>

/** @brief Maximum length of an actor username, excluding the null terminator. */
constexpr size_t ACTOR_NAME_MAX = 32;

/** @brief Nature of the actor responsible for an action. */
enum class ActorKind : uint8_t {
    System = 0,  ///< Automation, scheduler, safety, boot or unauthenticated origin.
    User = 1,    ///< Signed-in human operator.
};

/** @brief Identity responsible for an action. */
struct Actor {
    ActorKind kind = ActorKind::System;
    char username[ACTOR_NAME_MAX] = {0};
};

/** @brief Build a System actor (no human identity). */
inline Actor systemActor()
{
    return Actor{};
}

/** @brief Build a User actor from an authenticated username. */
inline Actor userActor(const char* username)
{
    Actor actor{};
    actor.kind = ActorKind::User;
    if (username && username[0] != '\0') {
        size_t i = 0;
        for (; i + 1U < sizeof(actor.username) && username[i] != '\0'; ++i) {
            actor.username[i] = username[i];
        }
        actor.username[i] = '\0';
    }
    return actor;
}
