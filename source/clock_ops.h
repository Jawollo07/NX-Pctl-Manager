// NX-Pctl-Manager — network-clock helpers (issue #2).
//
// The play timer only counts down once the console's NetworkSystemClock is
// valid. A console that never reaches Nintendo's time servers (offline, or
// with Nintendo's hosts blocked — common on CFW) never gets one, so the limit
// is "on" but nothing happens. Seeding the network clock from the user clock
// once (what QuickNTP does over NTP) makes the timer start working.
// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#pragma once
#include <switch.h>

typedef struct {
    bool user_ok;      // UserSystemClock readable
    u64  user;         // POSIX seconds
    bool network_ok;   // NetworkSystemClock readable (false == never set)
    u64  network;
} ClockStatus;

void clock_status(ClockStatus *out);

// NetworkSystemClock := UserSystemClock. Needs a time:s session, which the
// __nx_time_service_type override in clock_ops.c provides under Atmosphère.
Result clock_seed_network_from_user(void);
