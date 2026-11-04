#include "../protocol/trusted_clock.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using nxpctl::trusted_clock::Anchor;
    using nxpctl::trusted_clock::GetLocalDayKey;

    // UTC day boundary follows the server anchor plus monotonic elapsed time.
    Anchor utc{86'399, 200'000, 500, 0, true};
    std::int64_t day = -1;
    assert(GetLocalDayKey(utc, 500, &day) && day == 0);
    assert(GetLocalDayKey(utc, 501, &day) && day == 1);

    // The configured fixed policy offset, not the console wall clock, decides
    // the local day boundary.
    Anchor cet{82'800, 200'000, 100, 60, true};
    assert(GetLocalDayKey(cet, 100, &day) && day == 1);

    Anchor west{86'399, 200'000, 0, -60, true};
    assert(GetLocalDayKey(west, 0, &day) && day == 0);

    // An expired policy, regressed monotonic clock, invalid anchor or null
    // output must never fabricate a trusted local date.
    assert(!GetLocalDayKey(utc, 114'102, &day));
    assert(!GetLocalDayKey(utc, 499, &day));
    Anchor invalid = utc;
    invalid.valid = false;
    assert(!GetLocalDayKey(invalid, 500, &day));
    assert(!GetLocalDayKey(utc, 500, nullptr));

    std::cout << "trusted clock day-key tests OK\n";
}
