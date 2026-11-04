#include "../protocol/usage_tracker.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using namespace nxpctl::usage;

    DailyUsageTracker tracker;
    assert(!tracker.HasBaseline());
    assert(tracker.GetDailyUsageMinutes() == 0);

    // Start with no application. The first launch sample must not charge an
    // interval whose start time is unknown.
    assert(tracker.Observe(false, 100, 20) == ObserveResult::BaselineEstablished);
    assert(tracker.Observe(true, 101, 20) == ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 0);

    // All running games/apps share one global counter.
    assert(tracker.Observe(true, 102, 20) == ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 1);
    assert(tracker.GetDailyUsageMinutes() == 1); // round up started minutes
    assert(tracker.Observe(true, 103, 20) == ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 2); // switching title changes nothing

    // Leaving the last running application charges the final active interval.
    assert(tracker.Observe(false, 104, 20) == ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 3);
    assert(tracker.Observe(false, 105, 20) == ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 3);

    // Large gaps are not silently counted as continuous playtime.
    assert(tracker.Observe(true, 110, 20) == ObserveResult::ObservationGapTooLarge);
    assert(tracker.GetActiveSeconds() == 3);
    assert(tracker.Observe(true, 111, 20) == ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 4);

    // A forward local-day transition resets the daily bucket and does not
    // charge the interval that crosses the day boundary.
    assert(tracker.Observe(true, 112, 21) == ObserveResult::DayRolledOver);
    assert(tracker.GetActiveSeconds() == 0);
    assert(tracker.GetDailyUsageMinutes() == 0);
    assert(tracker.GetLocalDayKey() == 21);
    assert(tracker.Observe(true, 113, 21) == ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 1);

    // Backward wall-clock corrections and monotonic-clock regressions cannot
    // erase already accumulated time.
    assert(tracker.Observe(true, 114, 20) == ObserveResult::ClockRegressed);
    assert(tracker.GetActiveSeconds() == 1);
    assert(tracker.GetLocalDayKey() == 21);
    assert(tracker.Observe(true, 1, 21) == ObserveResult::ClockRegressed);
    assert(tracker.GetActiveSeconds() == 1);
    assert(tracker.Observe(true, 2, 21) == ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 2);

    DailyUsageTracker restored;
    assert(restored.Restore(42, 3599, 9000));
    assert(restored.GetActiveSeconds() == 3599);
    assert(restored.Observe(true, 9001, 42) == ObserveResult::Accumulated);
    assert(restored.GetActiveSeconds() == 3599);
    assert(restored.Observe(true, 9002, 42) == ObserveResult::Accumulated);
    assert(restored.GetActiveSeconds() == 3600);
    assert(!restored.Restore(-1, 10, 0));
    assert(!restored.Restore(42, MaxPersistedActiveSeconds + 1, 0));

    tracker.Reset();
    assert(!tracker.HasBaseline());
    assert(tracker.GetActiveSeconds() == 0);
    assert(tracker.GetDailyUsageMinutes() == 0);

    std::cout << "global daily usage tracker OK; max_gap_seconds="
              << MaxObservationGapSeconds << "; started_minutes_round_up\n";
}
