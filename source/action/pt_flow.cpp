// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#include "action/pt_flow.hpp"

#include <borealis.hpp>
#include "util/trf.hpp"

using namespace brls::literals;

namespace pt_flow
{

namespace
{
void show_dialog(const std::string& text)
{
    auto* d = new brls::Dialog(text);
    d->addButton("hints/ok"_i18n, [] {});
    d->open();
}
}   // namespace

bool guarded_write(const std::function<Result()>& write, const std::string& ok_text)
{
    if (!pctl_fw_play_timer_supported()) {
        u32 hv = hosversionGet();
        show_dialog(trf("nx_pctl/play_timer/fw_unsupported"_i18n,
                        HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv)));
        return false;
    }

    PctlStatus st;
    pctl_status_fetch(&st);
    if (!st.pin_length_ok || st.pin_length == 0) {
        show_dialog("nx_pctl/play_timer/need_pin"_i18n);
        return false;
    }

    PtState pt;
    pctl_play_timer_query(&pt);
    bool relock = false;
    if (pt.enabled || pt.restricted) {
        Result urc = pctl_unlock_restriction_temporarily();
        PtState after;
        pctl_play_timer_query(&after);
        if (R_FAILED(urc) || after.enabled) {
            // Don't risk writing while the timer is still live.
            brls::Application::notify(trf("nx_pctl/play_timer/unlock_failed"_i18n, (unsigned)urc));
            if (R_SUCCEEDED(urc)) pctl_revert_temporary_unlock();
            return false;
        }
        relock = true;
    } else {
        // Already paused (from the HOME menu, or left that way by v3.0.0): the
        // new settings wouldn't apply until parental controls are back on.
        bool paused = false;
        relock = R_SUCCEEDED(pctl_is_temporarily_unlocked(&paused)) && paused;
    }

    Result rc = write();

    bool relocked = true;
    if (relock) {
        Result rrc   = pctl_revert_temporary_unlock();
        bool   still = true;   // a failed 1006 query counts as still paused
        relocked = R_SUCCEEDED(rrc) && R_SUCCEEDED(pctl_is_temporarily_unlocked(&still)) && !still;
    }

    if (R_FAILED(rc))
        brls::Application::notify(trf("nx_pctl/play_timer/write_failed"_i18n, (unsigned)rc));
    else
        brls::Application::notify(ok_text);

    // Fallback to the v3.0.0 instructions if the automatic re-enable didn't stick.
    if (!relocked) show_dialog("nx_pctl/play_timer/relock_failed"_i18n);
    return R_SUCCEEDED(rc);
}

}   // namespace pt_flow
