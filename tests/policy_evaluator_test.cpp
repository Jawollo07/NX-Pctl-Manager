#include "../protocol/policy_evaluator.hpp"

#include <cassert>
#include <iostream>

namespace {
using namespace nxpctl::policy;
Policy BasePolicy() {
    Policy p{};
    p.enabled = true;
    p.global_daily_limit_minutes = 120;
    for (auto& day : p.weekday_limit_minutes) day = NoDailyLimit;
    p.offline_behavior = OfflineBehavior::KeepLastVerified;
    return p;
}
EvaluationContext At(std::uint8_t weekday = 0, std::uint16_t minute = 600) {
    EvaluationContext c{};
    c.weekday = weekday;
    c.minute_of_day = minute;
    return c;
}
}

int main() {
    using namespace nxpctl::policy;
    auto p = BasePolicy();
    auto c = At();
    assert(Evaluate(p, c).allowed);

    p.enabled = false;
    assert(Evaluate(p, c).reason == DecisionReason::PolicyDisabled);
    p.enabled = true;

    p.require_parent_authentication = true;
    assert(Evaluate(p, c).reason == DecisionReason::ParentAuthenticationRequired);
    c.parent_authenticated = true;
    assert(Evaluate(p, c).allowed);
    p.require_parent_authentication = false;

    p.quiet_hours_enabled = true;
    p.quiet_hours_start_minute = 22 * 60;
    p.quiet_hours_end_minute = 6 * 60;
    assert(Evaluate(p, At(0, 23 * 60)).reason == DecisionReason::QuietHours);
    assert(Evaluate(p, At(0, 5 * 60)).reason == DecisionReason::QuietHours);
    assert(Evaluate(p, At(0, 12 * 60)).allowed);
    p.quiet_hours_enabled = false;

    p.weekday_limit_minutes[0] = 60;
    auto used = At();
    used.daily_usage_minutes = 60;
    assert(Evaluate(p, used).reason == DecisionReason::DailyLimitReached);
    used.weekday = 1;
    used.daily_usage_minutes = 119;
    assert(Evaluate(p, used).allowed);
    used.daily_usage_minutes = 120;
    assert(Evaluate(p, used).reason == DecisionReason::DailyLimitReached);
    p.global_daily_limit_minutes = NoDailyLimit;
    used.daily_usage_minutes = 1440;
    assert(Evaluate(p, used).allowed);

    p.quiet_hours_enabled = true;
    p.quiet_hours_start_minute = 22 * 60;
    p.quiet_hours_end_minute = 6 * 60;
    assert(Evaluate(p, At(0, 23 * 60)).reason == DecisionReason::QuietHours);
    p.quiet_hours_enabled = false;

    p.offline_behavior = OfflineBehavior::BlockAllUsage;
    auto offline = At();
    offline.offline = true;
    assert(Evaluate(p, offline).reason == DecisionReason::OfflineBlockAllUsage);
    p.offline_behavior = OfflineBehavior::KeepLastVerified;
    assert(Evaluate(p, offline).allowed);

    assert(Evaluate(p, At(7, 600)).reason == DecisionReason::InvalidContext);
    assert(Evaluate(p, At(0, 1440)).reason == DecisionReason::InvalidContext);

    // Unix-time adapter derives Monday=0 weekday and minute-of-day from the
    // authenticated local timestamp, without consulting a wall clock.
    p.weekday_limit_minutes[0] = NoDailyLimit;
    p.global_daily_limit_minutes = 120;
    const std::int64_t monday_10am = 4 * 86400 + 10 * 3600;
    assert(EvaluateAtLocalUnixTime(p, monday_10am, 119, false, false).allowed);
    assert(EvaluateAtLocalUnixTime(p, monday_10am, 120, false, false).reason
           == DecisionReason::DailyLimitReached);
    p.quiet_hours_enabled = true;
    p.quiet_hours_start_minute = 22 * 60;
    p.quiet_hours_end_minute = 6 * 60;
    const std::int64_t monday_23pm = 4 * 86400 + 23 * 3600;
    assert(EvaluateAtLocalUnixTime(p, monday_23pm, 0, false, false).reason
           == DecisionReason::QuietHours);
    assert(EvaluateAtLocalUnixTime(p, -1, 0, false, false).reason
           == DecisionReason::InvalidContext);

    std::cout << "policy evaluator OK; weekdays=7; quiet_hours=midnight-safe\n";
}
