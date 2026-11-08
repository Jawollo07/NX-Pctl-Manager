#include "../protocol/connectivity.hpp"

#include <cassert>
#include <cstdint>
#include <limits>
#include <iostream>

int main() {
    using namespace nxpctl::connectivity;

    assert(GetState(false, 0, 1000) == State::Unknown);
    assert(IsOffline(false, 0, 1000));
    assert(GetSuccessAge(false, 0, 1000) == std::numeric_limits<std::uint64_t>::max());

    assert(GetState(true, 1000, 1000) == State::Online);
    assert(GetState(true, 1000, 1120) == State::Online);
    assert(!IsOffline(true, 1000, 1120));
    assert(GetSuccessAge(true, 1000, 1120) == 120);

    assert(GetState(true, 1000, 1121) == State::Offline);
    assert(IsOffline(true, 1000, 1121));
    assert(GetSuccessAge(true, 1000, 1121) == 121);

    // A monotonic regression is never interpreted as a fresh online session.
    assert(GetState(true, 1000, 999) == State::Unknown);
    assert(IsOffline(true, 1000, 999));
    assert(GetSuccessAge(true, 1000, 999) == std::numeric_limits<std::uint64_t>::max());

    std::cout << "connectivity state OK; grace=120s; unknown/offline fail-safe\n";
}
