// NX-Pctl-Manager — network-clock helpers (see clock_ops.h).
// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#include "clock_ops.h"
#include "pctl_ops.h"
#include <string.h>

// Make libnx's startup open time:s instead of time:u, so SetCurrentTime on the
// network clock is allowed (same approach as QuickNTP).
TimeServiceType __nx_time_service_type = TimeServiceType_System;

void clock_status(ClockStatus *out)
{
    memset(out, 0, sizeof(*out));
    out->user_ok    = R_SUCCEEDED(timeGetCurrentTime(TimeType_UserSystemClock,    &out->user));
    out->network_ok = R_SUCCEEDED(timeGetCurrentTime(TimeType_NetworkSystemClock, &out->network));
}

Result clock_seed_network_from_user(void)
{
    u64 now = 0;
    Result rc = timeGetCurrentTime(TimeType_UserSystemClock, &now);
    if (R_SUCCEEDED(rc)) rc = timeSetCurrentTime(TimeType_NetworkSystemClock, now);
    pctl_logf("seed NetworkSystemClock from UserSystemClock (%llu) rc=0x%08X",
              (unsigned long long)now, (unsigned)rc);
    return rc;
}
