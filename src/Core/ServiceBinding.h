#pragma once
/**
 * @file ServiceBinding.h
 * @brief Compile-time helpers to bind `void* ctx` service slots to instance methods.
 */

#include <type_traits>
#include <utility>

namespace ServiceBinding {

template<auto Method>
struct BindImpl;

template<typename T, typename R, typename... Args, R (T::*Method)(Args...)>
struct BindImpl<Method> {
    static_assert(!std::is_reference<R>::value, "ServiceBinding does not support reference-returning methods");

    static R call(void* ctx, Args... args)
    {
        T* self = static_cast<T*>(ctx);
        if (!self) {
            if constexpr (!std::is_void<R>::value) {
                return R{};
            } else {
                return;
            }
        }
        return (self->*Method)(std::forward<Args>(args)...);
    }
};

template<typename T, typename R, typename... Args, R (T::*Method)(Args...) const>
struct BindImpl<Method> {
    static_assert(!std::is_reference<R>::value, "ServiceBinding does not support reference-returning methods");

    static R call(void* ctx, Args... args)
    {
        const T* self = static_cast<const T*>(ctx);
        if (!self) {
            if constexpr (!std::is_void<R>::value) {
                return R{};
            } else {
                return;
            }
        }
        return (self->*Method)(std::forward<Args>(args)...);
    }
};

template<auto Method, auto Fallback>
struct BindOrImpl;

template<typename T, typename R, typename... Args, R (T::*Method)(Args...), R Fallback>
struct BindOrImpl<Method, Fallback> {
    static_assert(!std::is_void<R>::value, "ServiceBinding::bind_or does not support void-returning methods");
    static_assert(!std::is_reference<R>::value, "ServiceBinding does not support reference-returning methods");

    static R call(void* ctx, Args... args)
    {
        T* self = static_cast<T*>(ctx);
        if (!self) return Fallback;
        return (self->*Method)(std::forward<Args>(args)...);
    }
};

template<typename T, typename R, typename... Args, R (T::*Method)(Args...) const, R Fallback>
struct BindOrImpl<Method, Fallback> {
    static_assert(!std::is_void<R>::value, "ServiceBinding::bind_or does not support void-returning methods");
    static_assert(!std::is_reference<R>::value, "ServiceBinding does not support reference-returning methods");

    static R call(void* ctx, Args... args)
    {
        const T* self = static_cast<const T*>(ctx);
        if (!self) return Fallback;
        return (self->*Method)(std::forward<Args>(args)...);
    }
};

template<auto Method>
inline constexpr auto bind = &BindImpl<Method>::call;

template<auto Method, auto Fallback>
inline constexpr auto bind_or = &BindOrImpl<Method, Fallback>::call;

} // namespace ServiceBinding
