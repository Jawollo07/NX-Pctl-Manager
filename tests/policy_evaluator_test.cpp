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
    c.title_id = 0x0100000000001000ULL;
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

    p.title_rule_count = 1;
    p.title_rules[0] = {c.title_id, TitleRuleAction::Block};
    assert(Evaluate(p, At()).reason == DecisionReason::TitleBlocked);
    p.title_rules[0].action = TitleRuleAction::Allow;
    assert(Evaluate(p, At()).allowed);

    p.title_rule_mode = TitleRuleMode::AllowListedOnly;
    p.title_rule_count = 0;
    assert(Evaluate(p, At()).reason == DecisionReason::TitleNotAllowListed);
    p.title_rule_count = 1;
    p.title_rules[0] = {c.title_id, TitleRuleAction::Allow};
    assert(Evaluate(p, At()).allowed);
    p.title_rules[0].action = TitleRuleAction::Block;
    assert(Evaluate(p, At()).reason == DecisionReason::TitleBlocked);
    p.title_rule_mode = TitleRuleMode::AllowAll;

    p.quiet_hours_enabled = true;
    p.quiet_hours_start_minute = 22 * 60;
    p.quiet_hours_end_minute = 6 * 60;
    p.title_rules[0].action = TitleRuleAction::Allow;
    assert(Evaluate(p, At(0, 23 * 60)).reason == DecisionReason::QuietHours);
    p.quiet_hours_enabled = false;

    p.offline_behavior = OfflineBehavior::BlockAllTitles;
    auto offline = At();
    offline.offline = true;
    offline.title_already_running = true;
    assert(Evaluate(p, offline).reason == DecisionReason::OfflineBlockAll);
    p.offline_behavior = OfflineBehavior::BlockNewTitles;
    offline.title_already_running = false;
    assert(Evaluate(p, offline).reason == DecisionReason::OfflineBlockNewTitle);
    offline.title_already_running = true;
    assert(Evaluate(p, offline).allowed);
    p.offline_behavior = OfflineBehavior::KeepLastVerified;
    assert(Evaluate(p, offline).allowed);

    assert(Evaluate(p, At(7, 600)).reason == DecisionReason::InvalidContext);
    assert(Evaluate(p, At(0, 1440)).reason == DecisionReason::InvalidContext);
    auto invalid = At();
    invalid.title_id = 0;
    assert(Evaluate(p, invalid).reason == DecisionReason::InvalidContext);

    std::cout << "policy evaluator OK; weekdays=7; quiet_hours=midnight-safe\n";
}
