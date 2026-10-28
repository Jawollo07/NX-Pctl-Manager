// Copyright (C) 2026 Taylor. GPLv3-or-later (see LICENSE).
#include "action/pt_flow.hpp"

#include "util/pctl_ops_c.hpp"

namespace pt_flow
{

bool guarded_write(const std::function<Result()>& write, const std::string& ok_text)
{
    (void)ok_text; // UI feedback belongs to the active frontend, not this service layer.

    if (!pctl_fw_play_timer_supported()) {
        const u32 hv = hosversionGet();
        pctl_logf("PLAY TIMER WRITE: refused; firmware %u.%u.%u lacks required commands",
                  HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv));
        return false;
    }

    PctlStatus st{};
    pctl_status_fetch(&st);
    if (!st.pin_length_ok || st.pin_length == 0) {
        pctl_logf("PLAY TIMER WRITE: refused; no parental-control PIN is registered");
        return false;
    }

    PtState pt{};
    pctl_play_timer_query(&pt);
    bool relock = false;
    if (pt.enabled || pt.restricted) {
        Result urc = pctl_unlock_restriction_temporarily();
        PtState after{};
        pctl_play_timer_query(&after);
        if (R_FAILED(urc) || after.enabled) {
            // Don't risk writing while the timer is still live.
            pctl_logf("PLAY TIMER WRITE: temporary unlock failed rc=0x%08X enabled_after=%d",
                      (unsigned)urc, after.enabled);
            if (R_SUCCEEDED(urc)) pctl_revert_temporary_unlock();
            return false;
        }
        relock = true;
    } else {
        // Also restore parental controls if they were already temporarily paused.
        bool paused = false;
        relock = R_SUCCEEDED(pctl_is_temporarily_unlocked(&paused)) && paused;
    }

    const Result rc = write();

    bool relocked = true;
    if (relock) {
        const Result rrc = pctl_revert_temporary_unlock();
        bool still = true; // A failed query counts as still paused.
        relocked = R_SUCCEEDED(rrc)
            && R_SUCCEEDED(pctl_is_temporarily_unlocked(&still)) && !still;
        if (!relocked)
            pctl_logf("PLAY TIMER WRITE: failed to restore parental controls rc=0x%08X",
                      (unsigned)rrc);
    }

    if (R_FAILED(rc))
        pctl_logf("PLAY TIMER WRITE: operation failed rc=0x%08X", (unsigned)rc);
    else
        pctl_logf("PLAY TIMER WRITE: operation succeeded");

    if (!relocked)
        pctl_logf("PLAY TIMER WRITE: parental controls may remain temporarily disabled");

    return R_SUCCEEDED(rc);
}

} // namespace pt_flow
