// Pure, deterministic policy decision logic. No Horizon APIs or side effects.
#pragma once

#include "policy.hpp"

#include <cstdint>

namespace nxpctl::policy {

enum class DecisionReason : std::uint8_t {
    Allowed = 0,
    PolicyDisabled,
    ParentAuthenticationRequired,
    OfflineBlockAllUsage,
    QuietHours,
    DailyLimitReached,
    InvalidContext,
};

struct EvaluationContext {
    // Monday = 0 ... Sunday = 6, matching weekday_limit_minutes.
    std::uint8_t weekday = 0;
    std::uint16_t minute_of_day = 0;
    std::uint16_t daily_usage_minutes = 0;
    bool offline = false;
    bool parent_authenticated = false;
};

struct Decision {
    bool allowed = false;
    DecisionReason reason = DecisionReason::InvalidContext;
};

[[nodiscard]] inline bool IsWithinQuietHours(
    const Policy& policy, std::uint16_t minute_of_day) noexcept {
    if (!policy.quiet_hours_enabled) return false;
    const auto start = policy.quiet_hours_start_minute;
    const auto end = policy.quiet_hours_end_minute;
    if (start < end) return minute_of_day >= start && minute_of_day < end;
    return minute_of_day >= start || minute_of_day < end;
}

[[nodiscard]] inline std::uint16_t EffectiveDailyLimit(
    const Policy& policy, std::uint8_t weekday) noexcept {
    if (weekday >= 7) return 0;
    const auto override_limit = policy.weekday_limit_minutes[weekday];
    return override_limit == NoDailyLimit
        ? policy.global_daily_limit_minutes
        : override_limit;
}

[[nodiscard]] inline Decision Evaluate(
    const Policy& policy, const EvaluationContext& context) noexcept {
    if (context.weekday >= 7 || context.minute_of_day >= 1440) {
        return {false, DecisionReason::InvalidContext};
    }
    if (!policy.enabled) return {true, DecisionReason::PolicyDisabled};
    if (policy.require_parent_authentication && !context.parent_authenticated) {
        return {false, DecisionReason::ParentAuthenticationRequired};
    }
    if (context.offline) {
        if (policy.offline_behavior == OfflineBehavior::BlockAllUsage) {
            return {false, DecisionReason::OfflineBlockAllUsage};
        }
    }

    if (IsWithinQuietHours(policy, context.minute_of_day)) {
        return {false, DecisionReason::QuietHours};
    }
    const auto daily_limit = EffectiveDailyLimit(policy, context.weekday);
    if (daily_limit != NoDailyLimit && context.daily_usage_minutes >= daily_limit) {
        return {false, DecisionReason::DailyLimitReached};
    }
    return {true, DecisionReason::Allowed};
}

} // namespace nxpctl::policy
