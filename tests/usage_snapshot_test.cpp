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
    Snapshot duplicate_generation{100, 1801, 8, true};
    assert(SelectNewest(newest, duplicate_generation) == nullptr);
    Snapshot identical_generation{100, 1800, 8, true};
    assert(SelectNewest(newest, identical_generation) == &newest);
    assert(SelectNewest(empty, corrupt) == nullptr);
    WritePlan plan{};
    assert(MakeWritePlan(empty, empty, &plan));
    assert(plan.valid && plan.target_slot == 0 && plan.sequence == 1);

    // A failed write leaves the previous committed slot untouched. Retrying
    // produces the same plan; only a successful write advances the sequence.
    Snapshot slot0{100, 900, 4, true};
    Snapshot slot1{100, 1200, 5, true};
    assert(MakeWritePlan(slot0, slot1, &plan));
    assert(plan.target_slot == 0 && plan.sequence == 6);
    const Snapshot before_failure0 = slot0;
    const Snapshot before_failure1 = slot1;
    const bool simulated_write_succeeded = false;
    if (simulated_write_succeeded) slot0 = Snapshot{100, 1500, plan.sequence, true};
    assert(slot0.sequence == before_failure0.sequence);
    assert(slot1.sequence == before_failure1.sequence);
    WritePlan retry{};
    assert(MakeWritePlan(slot0, slot1, &retry));
    assert(retry.target_slot == plan.target_slot && retry.sequence == plan.sequence);

    slot0 = Snapshot{100, 1500, retry.sequence, true};
    assert(SelectNewest(slot0, slot1) == &slot0);
    assert(MakeWritePlan(slot0, slot1, &plan));
    assert(plan.target_slot == 1 && plan.sequence == 7);

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
