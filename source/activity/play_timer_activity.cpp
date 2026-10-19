// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#include "activity/play_timer_activity.hpp"

#include <cstdio>
#include <fmt/format.h>

#include "action/pt_flow.hpp"
#include "activity/play_timer_perday_activity.hpp"
#include "util/numpad.hpp"
#include "util/pctl_ops_c.hpp"
#include "util/trf.hpp"

using namespace brls::literals;

namespace
{
// If the seven days share one value, return that minute count; otherwise return
// 60 as a sensible default for the numpad to land on. PT_DAY_NOLIMIT counts as
// "no useful starting value" → fall back to 60.
uint16_t guess_uniform_seed()
{
    PtState pt;
    pctl_play_timer_query(&pt);
    if (!pt.valid) return 60;
    for (int i = 1; i < 7; i++)
        if (pt.day_min[i] != pt.day_min[0]) return 60;
    return (pt.day_min[0] == PT_DAY_NOLIMIT) ? 60 : pt.day_min[0];
}

std::string fmt_hm(uint8_t h, uint8_t m) { return fmt::format("{:02}:{:02}", h, m); }

// A daily limit on any day, or the bedtime alarm.
bool is_configured(const PtState& pt)
{
    if (pt.bedtime.enabled) return true;
    for (int i = 0; i < 7; i++)
        if (pt.day_min[i] != PT_DAY_NOLIMIT) return true;
    return false;
}

// Writes the bedtime alarm, then asks the system (1954 / 1956-1959) how it
// interpreted the write — confirms the PlayTimerSettings layout on this console.
void write_bedtime(const PtBedtime& bed)
{
    std::string ok_text = bed.enabled
        ? trf("nx_pctl/play_timer/bedtime/ok_on"_i18n, fmt_hm(bed.hour, bed.minute),
              fmt_hm(bed.reset_hour, bed.reset_minute))
        : "nx_pctl/play_timer/bedtime/ok_off"_i18n;
    if (!pt_flow::guarded_write([bed]() { return pctl_play_timer_set_bedtime(&bed); }, ok_text))
        return;

    PtBedtime sys;
    bool ok = false;
    pctl_bedtime_readback(&sys, &ok);
    if (!ok) return;   // getters unavailable — nothing to compare
    bool match = sys.enabled == bed.enabled
        && (!bed.enabled || (sys.hour == bed.hour && sys.minute == bed.minute
                             && sys.reset_hour == bed.reset_hour && sys.reset_minute == bed.reset_minute));
    pctl_logf("bedtime readback: enabled=%d %02u:%02u reset %02u:%02u  match=%d", sys.enabled,
              sys.hour, sys.minute, sys.reset_hour, sys.reset_minute, match);
    if (!match) {
        auto* d = new brls::Dialog(trf("nx_pctl/play_timer/bedtime/mismatch"_i18n,
                                       sys.enabled ? "on" : "off", fmt_hm(sys.hour, sys.minute),
                                       fmt_hm(sys.reset_hour, sys.reset_minute)));
        d->addButton("hints/ok"_i18n, [] {});
        d->open();
    }
}

void edit_bedtime()
{
    PtState pt;
    pctl_play_timer_query(&pt);
    numpad::HourMinute at_seed    = { 21, 0 };
    numpad::HourMinute reset_seed = { 6, 0 };
    if (pt.bedtime.enabled) {
        at_seed    = { pt.bedtime.hour, pt.bedtime.minute };
        reset_seed = { pt.bedtime.reset_hour, pt.bedtime.reset_minute };
    }

    const std::string invalid = "nx_pctl/play_timer/bedtime/invalid"_i18n;
    auto at = numpad::prompt_hhmm("nx_pctl/play_timer/bedtime/at"_i18n, invalid, at_seed);
    if (!at.has_value()) return;
    auto reset = numpad::prompt_hhmm("nx_pctl/play_timer/bedtime/reset"_i18n, invalid, reset_seed);
    if (!reset.has_value()) return;

    PtBedtime bed = { true, at->hour, at->minute, reset->hour, reset->minute };
    write_bedtime(bed);
}

}   // namespace

void PlayTimerActivity::onContentAvailable()
{
    // Set daily limit (all days) — numpad → guarded write (cmd 195101).
    this->pt_set_all->registerClickAction([this](brls::View*) {
        auto v = numpad::prompt_minutes("nx_pctl/play_timer/numpad_all"_i18n, guess_uniform_seed());
        if (!v.has_value()) return true;   // user cancelled
        uint16_t value = *v;

        std::string ok_text = value ? trf("nx_pctl/play_timer/ok_all"_i18n, value)
                                    : "nx_pctl/play_timer/ok_all_zero"_i18n;
        pt_flow::guarded_write([value]() { return pctl_play_timer_set_uniform(value); }, ok_text);
        this->refresh_all();
        return true;
    });

    // When time is up: suspend the software, or notify only (restrictionMode).
    this->pt_mode->registerClickAction([this](brls::View*) {
        PtState pt;
        pctl_play_timer_query(&pt);
        if (!is_configured(pt)) {   // the mode lives in the play-timer settings; nothing to attach it to yet
            brls::Application::notify("nx_pctl/play_timer/mode/need_limit"_i18n);
            return true;
        }
        auto* dialog = new brls::Dialog("nx_pctl/play_timer/mode/body"_i18n);
        dialog->addButton("hints/cancel"_i18n, [] {});
        dialog->addButton("nx_pctl/play_timer/mode/alarm"_i18n, [this]() {
            brls::sync([this]() {
                pt_flow::guarded_write([]() { return pctl_play_timer_set_mode(PT_MODE_ALARM); },
                                       "nx_pctl/play_timer/mode/ok_alarm"_i18n);
                this->refresh_all();
            });
        });
        dialog->addButton("nx_pctl/play_timer/mode/suspend"_i18n, [this]() {
            brls::sync([this]() {
                pt_flow::guarded_write([]() { return pctl_play_timer_set_mode(PT_MODE_SUSPEND); },
                                       "nx_pctl/play_timer/mode/ok_suspend"_i18n);
                this->refresh_all();
            });
        });
        dialog->open();
        return true;
    });

    // Per-day limits — push the staging sub-Activity.
    this->pt_per_day->registerClickAction([](brls::View*) {
        brls::Application::pushActivity(new PlayTimerPerDayActivity());
        return true;
    });

    // Bedtime alarm (issue #4) — set (two HHMM prompts) or turn off.
    this->pt_bedtime->registerClickAction([this](brls::View*) {
        PtState pt;
        pctl_play_timer_query(&pt);
        std::string current = pt.bedtime.enabled
            ? trf("nx_pctl/play_timer/bedtime/current_on"_i18n, fmt_hm(pt.bedtime.hour, pt.bedtime.minute),
                  fmt_hm(pt.bedtime.reset_hour, pt.bedtime.reset_minute))
            : "nx_pctl/play_timer/bedtime/current_off"_i18n;
        if (pt.bedtime_varies) current += "\n\n" + "nx_pctl/play_timer/bedtime/varies_note"_i18n;

        auto* dialog = new brls::Dialog(current);
        dialog->addButton("hints/cancel"_i18n, [] {});
        dialog->addButton("nx_pctl/play_timer/bedtime/turn_off"_i18n, [this]() {
            brls::sync([this]() {
                PtBedtime off = { false, 0, 0, 6, 0 };
                write_bedtime(off);
                this->refresh_all();
            });
        });
        dialog->addButton("nx_pctl/play_timer/bedtime/set"_i18n, [this]() {
            // Defer: open the keypad after the dialog has closed.
            brls::sync([this]() {
                edit_bedtime();
                this->refresh_all();
            });
        });
        dialog->open();
        return true;
    });

    // Remove play-time limit. Goes through the guarded write like every other
    // write: removing it while the "time's up" lock was up could crash
    // Atmosphère in v3.0.0, which wrote here without the temporary unlock.
    this->pt_remove->registerClickAction([this](brls::View*) {
        auto* dialog = new brls::Dialog("nx_pctl/play_timer/dialog/remove/body"_i18n);
        dialog->addButton("hints/cancel"_i18n, [] {});
        dialog->addButton("nx_pctl/play_timer/dialog/remove/confirm"_i18n, [this]() {
            brls::sync([this]() {
                pt_flow::guarded_write([]() { return pctl_play_timer_clear(); },
                                       "nx_pctl/play_timer/ok_removed"_i18n);
                this->refresh_all();
            });
        });
        dialog->open();
        return true;
    });

    this->refresh_all();
}

void PlayTimerActivity::onResume()
{
    brls::Activity::onResume();
    this->refresh_all();
}

void PlayTimerActivity::refresh_all()
{
    this->state_header->refresh();
    PtState pt;
    pctl_play_timer_query(&pt);
    // Nothing configured yet reads back as all-zero (ALARM), but the first
    // write defaults to suspending — show what will actually apply.
    this->pt_mode->setDetailText(is_configured(pt) && pt.restriction_mode == PT_MODE_ALARM
                                     ? "nx_pctl/play_timer/mode/alarm"_i18n
                                     : "nx_pctl/play_timer/mode/suspend"_i18n);
}
