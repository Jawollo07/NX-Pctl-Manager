#include "../protocol/policy.hpp"

#include <cassert>
#include <cstring>
#include <iostream>

namespace {
using namespace nxpctl::policy;

Policy MakePolicy(std::uint64_t revision = 1) {
    Policy value{};
    std::strcpy(value.device_id, "switch-device-01");
    value.revision = revision;
    value.issued_at = 1'000;
    value.not_after = 1'000 + 7 * 24 * 60 * 60;
    value.server_time = 1'000;
    value.global_daily_limit_minutes = 120;
    value.weekday_limit_minutes[0] = 60;
    value.quiet_hours_enabled = true;
    value.quiet_hours_start_minute = 22 * 60;
    value.quiet_hours_end_minute = 6 * 60;
    value.title_rule_count = 2;
    value.title_rules[0] = {0x0100000000001000ULL, TitleRuleAction::Allow};
    value.title_rules[1] = {0x0100000000002000ULL, TitleRuleAction::Block};
    return value;
}

ValidationContext Context(std::uint64_t minimum_revision = 0) {
    return {"switch-device-01", 1'000, minimum_revision};
}
}

int main() {
    using namespace nxpctl::policy;

    auto candidate = MakePolicy();
    assert(Validate(candidate, Context()) == ValidationError::None);

    auto invalid = candidate;
    invalid.protocol_version++;
    assert(Validate(invalid, Context()) == ValidationError::UnsupportedProtocol);

    invalid = candidate;
    invalid.schema_version++;
    assert(Validate(invalid, Context()) == ValidationError::UnsupportedSchema);

    invalid = candidate;
    invalid.revision = 0;
    assert(Validate(invalid, Context()) == ValidationError::StaleRevision);

    invalid = candidate;
    invalid.device_id[0] = 'X';
    assert(Validate(invalid, Context()) == ValidationError::DeviceMismatch);

    invalid = candidate;
    invalid.not_after = 999;
    assert(Validate(invalid, Context()) == ValidationError::InvalidTimestamps);

    invalid = candidate;
    invalid.not_after = 1'050;
    assert(Validate(invalid, {"switch-device-01", 1'100, 0}) == ValidationError::Expired);

    invalid = candidate;
    invalid.server_time += AllowedClockSkewSeconds + 1;
    assert(Validate(invalid, Context()) == ValidationError::ClockSkew);

    invalid = candidate;
    invalid.not_after = invalid.issued_at + MaxPolicyLifetimeSeconds + 1;
    assert(Validate(invalid, Context()) == ValidationError::LifetimeTooLong);

    invalid = candidate;
    invalid.timezone_offset_minutes = 841;
    assert(Validate(invalid, Context()) == ValidationError::InvalidTimezone);

    invalid = candidate;
    invalid.global_daily_limit_minutes = 1441;
    assert(Validate(invalid, Context()) == ValidationError::InvalidDailyLimit);

    invalid = candidate;
    invalid.quiet_hours_end_minute = invalid.quiet_hours_start_minute;
    assert(Validate(invalid, Context()) == ValidationError::InvalidQuietHours);

    invalid = candidate;
    invalid.title_rules[1].title_id = invalid.title_rules[0].title_id;
    assert(Validate(invalid, Context()) == ValidationError::DuplicateTitleRule);

    invalid = candidate;
    invalid.title_rules[0].title_id = 0;
    assert(Validate(invalid, Context()) == ValidationError::InvalidTitleRule);

    invalid = candidate;
    invalid.title_rule_mode = static_cast<TitleRuleMode>(2);
    assert(Validate(invalid, Context()) == ValidationError::InvalidTitleRule);

    invalid = candidate;
    invalid.title_rule_count = MaxTitleRules + 1;
    assert(Validate(invalid, Context()) == ValidationError::TooManyTitleRules);

    PolicyStore store;
    assert(!store.HasPolicy());
    assert(store.GetRevision() == 0);
    assert(store.TryReplace(candidate, Context()) == ValidationError::None);
    assert(store.HasPolicy());
    assert(store.GetRevision() == 1);

    Policy copied{};
    assert(store.CopyActive(&copied));
    assert(copied.revision == 1);
    assert(std::strcmp(copied.device_id, "switch-device-01") == 0);

    // A rejected replacement must leave the complete active snapshot intact.
    auto stale = MakePolicy(1);
    stale.global_daily_limit_minutes = 5;
    assert(store.TryReplace(stale, Context()) == ValidationError::StaleRevision);
    assert(store.CopyActive(&copied));
    assert(copied.revision == 1);
    assert(copied.global_daily_limit_minutes == 120);

    auto next = MakePolicy(2);
    next.global_daily_limit_minutes = 90;
    assert(store.TryReplace(next, Context()) == ValidationError::None);
    assert(store.GetRevision() == 2);
    assert(store.CopyActive(&copied));
    assert(copied.revision == 2);
    assert(copied.global_daily_limit_minutes == 90);

    assert(!store.CopyActive(nullptr));

    std::cout << "policy validation and RAM snapshot store OK; max_title_rules="
              << MaxTitleRules << '\n';
}
