#include "../protocol/policy_json.hpp"

#include <cassert>
#include <cstring>
#include <iostream>
#include <string>

namespace {
using namespace nxpctl::policy;
constexpr const char* ValidJson =
    R"({"protocol_version":1,"schema_version":1,"device_id":"switch-device-01","revision":4,"issued_at":1000,"not_after":605800,"server_time":1000,"enabled":true,"require_parent_authentication":false,"offline_behavior":0,"timezone_offset_minutes":60,"global_daily_limit_minutes":120,"weekday_limit_minutes":[60,60,60,60,90,120,120],"quiet_hours_enabled":true,"quiet_hours_start_minute":1320,"quiet_hours_end_minute":360})";

ValidationContext Context() { return {"switch-device-01", 1000, 3}; }
}

int main() {
    using namespace nxpctl::policy;
    using namespace nxpctl::policy::json;

    Policy decoded{};
    assert(DecodeAndValidate(ValidJson, std::strlen(ValidJson), Context(), &decoded) == Error::None);
    assert(decoded.revision == 4);
    assert(decoded.timezone_offset_minutes == 60);
    assert(decoded.weekday_limit_minutes[4] == 90);

    const auto expect = [&](const std::string& input, Error expected) {
        Policy output{};
        assert(Decode(input.data(), input.size(), &output) == expected);
    };

    auto unknown = std::string(ValidJson);
    unknown.insert(1, R"("title_rules":[],)");
    expect(unknown, Error::UnknownField);
    unknown = std::string(ValidJson);
    unknown.insert(1, R"("unexpected":1,)");
    expect(unknown, Error::UnknownField);

    auto duplicate = std::string(ValidJson);
    duplicate.insert(1, R"("revision":9,)");
    expect(duplicate, Error::DuplicateField);

    auto escaped = std::string(ValidJson);
    const auto id = escaped.find("switch-device-01");
    escaped.replace(id, std::strlen("switch-device-01"), "switch\\u002ddevice-01");
    expect(escaped, Error::UnsupportedStringEscape);

    auto fractional = std::string(ValidJson);
    const auto revision = fractional.find(R"("revision":4)");
    fractional.replace(revision, std::strlen(R"("revision":4)"), R"("revision":4.5)");
    expect(fractional, Error::InvalidNumber);

    auto wrong_array = std::string(ValidJson);
    const auto weekdays = wrong_array.find("[60,60,60,60,90,120,120]");
    wrong_array.replace(weekdays, std::strlen("[60,60,60,60,90,120,120]"), "[60,60]");
    expect(wrong_array, Error::ArraySize);

    std::string too_large(MaxDocumentBytes + 1, ' ');
    expect(too_large, Error::DocumentTooLarge);

    PolicyStore store;
    assert(store.TryReplace(decoded, Context()) == ValidationError::None);
    auto rejected = std::string(ValidJson);
    const auto rev = rejected.find(R"("revision":4)");
    rejected.replace(rev, std::strlen(R"("revision":4)"), R"("revision":5)");
    Policy candidate{};
    assert(DecodeAndValidate(rejected.data(), rejected.size(), Context(), &candidate) == Error::None);
    assert(store.TryReplace(candidate, Context()) == ValidationError::None);
    assert(store.GetRevision() == 5);

    std::cout << "bounded policy JSON decoder OK; bytes=" << std::strlen(ValidJson) << '\n';
}
