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
    pctl_logf("CLOCK STATUS: entered");
    memset(out, 0, sizeof(*out));
    Result user_rc = timeGetCurrentTime(TimeType_UserSystemClock, &out->user);
    Result network_rc = timeGetCurrentTime(TimeType_NetworkSystemClock, &out->network);
    out->user_ok = R_SUCCEEDED(user_rc);
    out->network_ok = R_SUCCEEDED(network_rc);
    pctl_logf("CLOCK STATUS: user_rc=0x%08X network_rc=0x%08X user_ok=%d network_ok=%d",
              (unsigned)user_rc, (unsigned)network_rc, out->user_ok, out->network_ok);
    pctl_logf("CLOCK STATUS: completed");
}

Result clock_seed_network_from_user(void)
{
    pctl_logf("CLOCK SEED: entered");
    u64 now = 0;
    Result rc = timeGetCurrentTime(TimeType_UserSystemClock, &now);
    pctl_logf("CLOCK SEED: read user clock rc=0x%08X", (unsigned)rc);
    if (R_SUCCEEDED(rc)) {
        pctl_logf("CLOCK SEED: setting network clock from user clock");
        rc = timeSetCurrentTime(TimeType_NetworkSystemClock, now);
        pctl_logf("CLOCK SEED: timeSetCurrentTime rc=0x%08X", (unsigned)rc);
    }
    pctl_logf("seed NetworkSystemClock from UserSystemClock (%llu) rc=0x%08X",
              (unsigned long long)now, (unsigned)rc);
    return rc;
}
