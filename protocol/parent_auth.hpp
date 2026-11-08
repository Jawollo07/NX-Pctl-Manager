// Volatile parent-authentication state machine built on the RFC 6238 verifier.
// Secret and replay counter must be loaded/saved by the encrypted platform store.
#pragma once

#include "totp.hpp"

#include <cstddef>
#include <cstdint>

namespace nxpctl::parent_auth {

inline constexpr std::size_t SecretCapacity = 64;
inline constexpr std::uint64_t SessionLifetimeSeconds = 15 * 60;
inline constexpr std::uint64_t LockoutSeconds = 5 * 60;
inline constexpr std::uint32_t MaxFailedAttempts = 5;

enum class Result : std::uint8_t {
    Authenticated = 0,
    InvalidCode,
    LockedOut,
    NotConfigured,
    InvalidTime,
};

class Authenticator {
public:
    Authenticator() = default;
    Authenticator(const Authenticator&) = delete;
    Authenticator& operator=(const Authenticator&) = delete;

    ~Authenticator() { Clear(); }

    [[nodiscard]] bool Configure(
        const std::uint8_t* secret, std::size_t secret_size,
        std::uint64_t last_accepted_counter = 0) noexcept {
        if (secret == nullptr || secret_size == 0 || secret_size > SecretCapacity)
            return false;
        Clear();
        for (std::size_t i = 0; i < secret_size; ++i) secret_[i] = secret[i];
        secret_size_ = secret_size;
        last_accepted_counter_ = last_accepted_counter;
        configured_ = true;
        return true;
    }

    void Clear() noexcept {
        volatile std::uint8_t* p = secret_;
        for (std::size_t i = 0; i < sizeof(secret_); ++i) p[i] = 0;
        secret_size_ = 0;
        last_accepted_counter_ = 0;
        failed_attempts_ = 0;
        lockout_until_ = 0;
        authenticated_until_ = 0;
        configured_ = false;
    }

    [[nodiscard]] Result Authenticate(
        const char* code, std::uint64_t trusted_unix_time,
        std::uint64_t monotonic_seconds) noexcept {
        if (!configured_) return Result::NotConfigured;
        if (trusted_unix_time == 0 || monotonic_seconds == 0)
            return Result::InvalidTime;
        if (monotonic_seconds < lockout_until_) return Result::LockedOut;
        if (lockout_until_ != 0) {
            lockout_until_ = 0;
            failed_attempts_ = 0;
        }

        std::uint64_t matched_counter = 0;
        if (!totp::Verify(secret_, secret_size_, trusted_unix_time, code,
                         last_accepted_counter_, &matched_counter)) {
            if (failed_attempts_ < MaxFailedAttempts) ++failed_attempts_;
            if (failed_attempts_ >= MaxFailedAttempts) {
                lockout_until_ = monotonic_seconds > UINT64_MAX - LockoutSeconds
                    ? UINT64_MAX : monotonic_seconds + LockoutSeconds;
                authenticated_until_ = 0;
                return Result::LockedOut;
            }
            return Result::InvalidCode;
        }

        last_accepted_counter_ = matched_counter;
        failed_attempts_ = 0;
        lockout_until_ = 0;
        authenticated_until_ = monotonic_seconds > UINT64_MAX - SessionLifetimeSeconds
            ? UINT64_MAX : monotonic_seconds + SessionLifetimeSeconds;
        return Result::Authenticated;
    }

    [[nodiscard]] bool IsAuthenticated(std::uint64_t monotonic_seconds) const noexcept {
        return configured_ && authenticated_until_ != 0
            && monotonic_seconds < authenticated_until_;
    }

    void EndSession() noexcept { authenticated_until_ = 0; }

    [[nodiscard]] std::uint64_t LastAcceptedCounter() const noexcept {
        return last_accepted_counter_;
    }

    [[nodiscard]] std::uint64_t LockoutUntil() const noexcept {
        return lockout_until_;
    }

    [[nodiscard]] std::uint64_t AuthenticatedUntil() const noexcept {
        return authenticated_until_;
    }

    [[nodiscard]] std::uint32_t FailedAttempts() const noexcept {
        return failed_attempts_;
    }

    [[nodiscard]] bool IsConfigured() const noexcept { return configured_; }

private:
    std::uint8_t secret_[SecretCapacity]{};
    std::size_t secret_size_ = 0;
    std::uint64_t last_accepted_counter_ = 0;
    std::uint64_t lockout_until_ = 0;
    std::uint64_t authenticated_until_ = 0;
    std::uint32_t failed_attempts_ = 0;
    bool configured_ = false;
};

} // namespace nxpctl::parent_auth
