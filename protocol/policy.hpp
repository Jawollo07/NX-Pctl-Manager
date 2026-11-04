// Versioned, Horizon-independent policy model and RAM snapshot store.
#pragma once

#include <atomic>
#include <cstddef>
#include <cstdint>

namespace nxpctl::policy {

inline constexpr std::uint32_t PolicyProtocolVersion = 1;
inline constexpr std::uint32_t PolicySchemaVersion = 1;
inline constexpr std::size_t DeviceIdCapacity = 65; // 64 bytes plus NUL.
inline constexpr std::uint16_t NoDailyLimit = 0xFFFF;
inline constexpr std::uint16_t MaxDailyLimitMinutes = 1440;
inline constexpr std::int64_t MaxPolicyLifetimeSeconds = 30LL * 24 * 60 * 60;
inline constexpr std::int64_t AllowedClockSkewSeconds = 300;

enum class OfflineBehavior : std::uint8_t {
    KeepLastVerified = 0,
    BlockAllUsage = 1,
};

struct Policy {
    std::uint32_t protocol_version = PolicyProtocolVersion;
    std::uint32_t schema_version = PolicySchemaVersion;
    char device_id[DeviceIdCapacity]{};
    std::uint64_t revision = 0;
    std::int64_t issued_at = 0;
    std::int64_t not_after = 0;
    std::int64_t server_time = 0;

    bool enabled = true;
    bool require_parent_authentication = false;
    OfflineBehavior offline_behavior = OfflineBehavior::KeepLastVerified;

    // Fixed UTC offset. DST/IANA timezone rules are deliberately not implied.
    std::int16_t timezone_offset_minutes = 0;
    std::uint16_t global_daily_limit_minutes = NoDailyLimit;
    // NoDailyLimit means inherit global_daily_limit_minutes for that weekday.
    std::uint16_t weekday_limit_minutes[7]{
        NoDailyLimit, NoDailyLimit, NoDailyLimit, NoDailyLimit,
        NoDailyLimit, NoDailyLimit, NoDailyLimit
    };

    bool quiet_hours_enabled = false;
    std::uint16_t quiet_hours_start_minute = 0;
    std::uint16_t quiet_hours_end_minute = 0;

};

enum class ValidationError : std::uint8_t {
    None = 0,
    NullPolicy,
    InvalidDeviceId,
    DeviceMismatch,
    UnsupportedProtocol,
    UnsupportedSchema,
    StaleRevision,
    InvalidTimestamps,
    ClockSkew,
    Expired,
    LifetimeTooLong,
    InvalidOfflineBehavior,
    InvalidTimezone,
    InvalidDailyLimit,
    InvalidQuietHours,
};

struct ValidationContext {
    const char* expected_device_id = nullptr;
    std::int64_t trusted_server_time = 0;
    std::uint64_t minimum_revision = 0;
};

[[nodiscard]] inline std::size_t BoundedStringLength(
    const char* value, std::size_t capacity) noexcept {
    if (value == nullptr) return capacity;
    for (std::size_t i = 0; i < capacity; ++i) {
        if (value[i] == '\0') return i;
    }
    return capacity;
}

[[nodiscard]] inline bool IsValidDailyLimit(std::uint16_t minutes) noexcept {
    return minutes == NoDailyLimit || minutes <= MaxDailyLimitMinutes;
}

[[nodiscard]] inline ValidationError Validate(
    const Policy& candidate, const ValidationContext& context) noexcept {
    const auto device_length = BoundedStringLength(candidate.device_id, DeviceIdCapacity);
    if (device_length == 0 || device_length >= DeviceIdCapacity) {
        return ValidationError::InvalidDeviceId;
    }

    const auto expected_length = BoundedStringLength(context.expected_device_id, DeviceIdCapacity);
    if (context.expected_device_id == nullptr || expected_length == 0
        || expected_length >= DeviceIdCapacity || expected_length != device_length) {
        return ValidationError::DeviceMismatch;
    }
    for (std::size_t i = 0; i < device_length; ++i) {
        if (candidate.device_id[i] != context.expected_device_id[i]) {
            return ValidationError::DeviceMismatch;
        }
    }

    if (candidate.protocol_version != PolicyProtocolVersion) {
        return ValidationError::UnsupportedProtocol;
    }
    if (candidate.schema_version != PolicySchemaVersion) {
        return ValidationError::UnsupportedSchema;
    }
    if (candidate.revision <= context.minimum_revision) {
        return ValidationError::StaleRevision;
    }

    if (candidate.issued_at <= 0 || candidate.not_after <= candidate.issued_at
        || candidate.server_time <= 0 || context.trusted_server_time <= 0) {
        return ValidationError::InvalidTimestamps;
    }
    if ((candidate.server_time > context.trusted_server_time
            && candidate.server_time - context.trusted_server_time > AllowedClockSkewSeconds)
        || (context.trusted_server_time > candidate.server_time
            && context.trusted_server_time - candidate.server_time > AllowedClockSkewSeconds)
        || (candidate.issued_at > candidate.server_time
            && candidate.issued_at - candidate.server_time > AllowedClockSkewSeconds)) {
        return ValidationError::ClockSkew;
    }
    if (candidate.not_after <= context.trusted_server_time) {
        return ValidationError::Expired;
    }
    if (candidate.not_after - candidate.issued_at > MaxPolicyLifetimeSeconds) {
        return ValidationError::LifetimeTooLong;
    }

    switch (candidate.offline_behavior) {
        case OfflineBehavior::KeepLastVerified:
        case OfflineBehavior::BlockAllUsage:
            break;
        default:
            return ValidationError::InvalidOfflineBehavior;
    }

    if (candidate.timezone_offset_minutes < -14 * 60
        || candidate.timezone_offset_minutes > 14 * 60) {
        return ValidationError::InvalidTimezone;
    }
    if (!IsValidDailyLimit(candidate.global_daily_limit_minutes)) {
        return ValidationError::InvalidDailyLimit;
    }
    for (const auto minutes : candidate.weekday_limit_minutes) {
        if (!IsValidDailyLimit(minutes)) return ValidationError::InvalidDailyLimit;
    }

    if (candidate.quiet_hours_enabled
        && (candidate.quiet_hours_start_minute >= 1440
            || candidate.quiet_hours_end_minute >= 1440
            || candidate.quiet_hours_start_minute == candidate.quiet_hours_end_minute)) {
        return ValidationError::InvalidQuietHours;
    }

    return ValidationError::None;
}

// Fixed-size snapshots avoid allocation and persistence on the console.
// Readers and writers serialize only the bounded in-memory copy; validation
// is repeated under the lock against the current revision before publication.
class PolicyStore {
public:
    [[nodiscard]] ValidationError TryReplace(
        const Policy& candidate, ValidationContext context) noexcept {
        LockGuard guard(lock_);
        if (has_policy_ && policy_.revision > context.minimum_revision) {
            context.minimum_revision = policy_.revision;
        }
        const auto result = Validate(candidate, context);
        if (result != ValidationError::None) return result;

        policy_ = candidate;
        has_policy_ = true;
        return ValidationError::None;
    }

    [[nodiscard]] bool CopyActive(Policy* out) const noexcept {
        if (out == nullptr) return false;
        LockGuard guard(lock_);
        if (!has_policy_) return false;
        *out = policy_;
        return true;
    }

    [[nodiscard]] std::uint64_t GetRevision() const noexcept {
        LockGuard guard(lock_);
        return has_policy_ ? policy_.revision : 0;
    }

    [[nodiscard]] bool HasPolicy() const noexcept {
        LockGuard guard(lock_);
        return has_policy_;
    }

private:
    class LockGuard {
    public:
        explicit LockGuard(std::atomic_flag& lock) noexcept : lock_(lock) {
            while (lock_.test_and_set(std::memory_order_acquire)) {
                // The critical section is a bounded fixed-size copy.
            }
        }
        ~LockGuard() { lock_.clear(std::memory_order_release); }
        LockGuard(const LockGuard&) = delete;
        LockGuard& operator=(const LockGuard&) = delete;
    private:
        std::atomic_flag& lock_;
    };

    mutable std::atomic_flag lock_ = ATOMIC_FLAG_INIT;
    Policy policy_{};
    bool has_policy_ = false;
};

} // namespace nxpctl::policy
