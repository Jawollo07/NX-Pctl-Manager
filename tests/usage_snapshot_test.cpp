#include "../protocol/usage_snapshot.hpp"
#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using namespace nxpctl::usage;
    Snapshot empty{};
    Snapshot old{100, 1200, 7, true};
    Snapshot newest{100, 1800, 8, true};
    Snapshot corrupt{100, MaxPersistedActiveSeconds + 1, 99, true};
    assert(SelectNewest(empty, old) == &old);
    assert(SelectNewest(old, newest) == &newest);
    assert(SelectNewest(newest, corrupt) == &newest);
    assert(SelectNewest(empty, corrupt) == nullptr);
    std::int64_t day = -1;
    std::uint64_t seconds = 0;
    assert(ReconcileSnapshot(newest, 100, &day, &seconds));
    assert(day == 100 && seconds == 1800);
    assert(ReconcileSnapshot(newest, 101, &day, &seconds));
    assert(day == 101 && seconds == 0);
    assert(!ReconcileSnapshot(newest, 99, &day, &seconds));
    assert(!ReconcileSnapshot(newest, 100, nullptr, &seconds));
    assert(!ReconcileSnapshot(newest, 100, &day, nullptr));
    std::cout << "usage snapshot selection and reconciliation OK\n";
}
