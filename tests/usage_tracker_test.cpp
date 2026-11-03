#include "../protocol/usage_tracker.hpp"

#include <cassert>
#include <cstdint>
#include <iostream>

int main() {
    using namespace nxpctl::usage;

    DailyUsageTracker tracker;
    assert(!tracker.HasBaseline());
    assert(tracker.GetDailyUsageMinutes() == 0);

    // Establish a baseline while no title is running; the first launch sample
    // must not charge an interval whose start time is unknown.
    assert(tracker.Observe(NoActiveTitle, 100, 20) ==
           ObserveResult::BaselineEstablished);
    assert(tracker.Observe(0x0100000000001000ULL, 101, 20) ==
           ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 0);

    // Charge only intervals whose previous sample had an active title.
    assert(tracker.Observe(0x0100000000001000ULL, 102, 20) ==
           ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 1);
    assert(tracker.GetDailyUsageMinutes() == 1); // round up started minutes

    // A title switch does not interrupt global daily usage accounting.
    assert(tracker.Observe(0x0100000000002000ULL, 103, 20) ==
           ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 2);

    // Leaving the application charges the final observed active interval.
    assert(tracker.Observe(NoActiveTitle, 104, 20) ==
           ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 3);
    assert(tracker.Observe(NoActiveTitle, 105, 20) ==
           ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 3);

    // Large gaps are not silently counted as continuous playtime.
    assert(tracker.Observe(0x0100000000001000ULL, 110, 20) ==
           ObserveResult::ObservationGapTooLarge);
    assert(tracker.GetActiveSeconds() == 3);
    assert(tracker.Observe(0x0100000000001000ULL, 111, 20) ==
           ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 4);

    // A forward local-day transition resets only the daily bucket and does
    // not charge the interval that crosses the day boundary.
    assert(tracker.Observe(0x0100000000001000ULL, 112, 21) ==
           ObserveResult::DayRolledOver);
    assert(tracker.GetActiveSeconds() == 0);
    assert(tracker.GetDailyUsageMinutes() == 0);
    assert(tracker.GetLocalDayKey() == 21);
    assert(tracker.Observe(0x0100000000001000ULL, 113, 21) ==
           ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 1);

    // Backward wall-clock corrections and monotonic-clock regressions cannot
    // erase already accumulated time.
    assert(tracker.Observe(0x0100000000001000ULL, 114, 20) ==
           ObserveResult::ClockRegressed);
    assert(tracker.GetActiveSeconds() == 1);
    assert(tracker.GetLocalDayKey() == 21);
    assert(tracker.Observe(0x0100000000001000ULL, 1, 21) ==
           ObserveResult::ClockRegressed);
    assert(tracker.GetActiveSeconds() == 1);
    assert(tracker.Observe(0x0100000000001000ULL, 2, 21) ==
           ObserveResult::Accumulated);
    assert(tracker.GetActiveSeconds() == 2);

    tracker.Reset();
    assert(!tracker.HasBaseline());
    assert(tracker.GetActiveSeconds() == 0);
    assert(tracker.GetDailyUsageMinutes() == 0);

    std::cout << "daily usage tracker OK; max_gap_seconds="
              << MaxObservationGapSeconds << "; started_minutes_round_up\n";
}
