// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#include "activity/main_activity.hpp"
#include "remote_control.hpp"

#include <array>
#include <cstdio>
#include <cstring>
#include <string>
#include <fmt/format.h>

#include "activity/play_timer_activity.hpp"
#include "app.hpp"
#include "util/numpad.hpp"
#include "util/pctl_ops_c.hpp"
#include "util/trf.hpp"

using namespace brls::literals;   // for "key"_i18n

namespace
{
// Common shape for the two destructive cells (delete-all / unlink):
//   show a confirm Dialog → on confirm run `action` → toast result → refresh the panel.
// `error_prefix` is the lead text for failure ("Could not X"); the Result code is appended.
template <typename Action, typename Panel>
void run_destructive(const std::string& body, const std::string& confirm_label,
                     const std::string& success_toast, const std::string& error_prefix,
                     Action action, Panel* panel)
{
    auto* dialog = new brls::Dialog(body);
    dialog->addButton("hints/cancel"_i18n, []() {});
    dialog->addButton(confirm_label, [=]() {
        Result rc = action();
        if (R_SUCCEEDED(rc))
            brls::Application::notify(success_toast);
        else
            brls::Application::notify(
                fmt::format("{} (error 0x{:08X}).", error_prefix, (unsigned)rc));
        panel->refresh();
    });
    dialog->open();
}
}   // namespace

void MainActivity::onContentAvailable()
{
    pctl_logf("MAIN ACTIVITY: onContentAvailable entered");

    // Refresh status — step (c): re-read pctl state and update the panel.
    // The status panel already refreshes on onResume; this lets the user force
    // a re-read without leaving the screen.
    pctl_logf("MAIN ACTIVITY: binding refresh click action");
    this->item_refresh->registerClickAction([this](brls::View*) {
        pctl_logf("UI ACTION: refresh clicked");
        this->status_panel->refresh();
        brls::Application::notify("nx_pctl/toast/status_refreshed"_i18n);
        return true;
    });

    // Synchronize with the central control server. Network failures are reported
    // without changing local settings; remote policy application is separately opt-in.
    pctl_logf("MAIN ACTIVITY: binding remote-control click action");
    this->item_remote_control->registerClickAction([](brls::View*) {
        pctl_logf("UI ACTION: remote-control sync clicked");
        const auto result = remote_control::synchronize();
        brls::Application::notify(result.message);
        return true;
    });

    // Launch authentication is handled by the root gate; no setup action is needed here.

    // Delete all parental controls (cmd 1043) — step (d), destructive, confirm-gated.
    pctl_logf("MAIN ACTIVITY: binding delete parental controls action");
    this->item_delete_pc->registerClickAction([this](brls::View*) {
        pctl_logf("UI ACTION: delete parental controls clicked");
        run_destructive(
            "nx_pctl/dialog/delete_pc/body"_i18n,
            "nx_pctl/dialog/delete_pc/confirm"_i18n,
            "nx_pctl/toast/delete_pc_ok"_i18n,
            "nx_pctl/toast/delete_pc_err"_i18n,
            pctl_delete_parental_controls,
            this->status_panel.getView());
        return true;
    });

    // Unlink companion app (cmd 1941) — step (d), destructive, confirm-gated.
    pctl_logf("MAIN ACTIVITY: binding unlink action");
    this->item_unlink->registerClickAction([this](brls::View*) {
        pctl_logf("UI ACTION: unlink clicked");
        run_destructive(
            "nx_pctl/dialog/unlink/body"_i18n,
            "nx_pctl/dialog/unlink/confirm"_i18n,
            "nx_pctl/toast/unlink_ok"_i18n,
            "nx_pctl/toast/unlink_err"_i18n,
            pctl_delete_pairing,
            this->status_panel.getView());
        return true;
    });

    // Play timer (daily limit) — step (e): push the sub-Activity.
    pctl_logf("MAIN ACTIVITY: binding play-timer action");
    this->item_play_timer->registerClickAction([](brls::View*) {
        pctl_logf("UI ACTION: play-timer clicked");
        brls::Application::pushActivity(new PlayTimerActivity());
        return true;
    });

    // Set / change PIN — step (f). pctl_set_pin() pops the pctlauth applet
    // (it's already wrapped in pctlExit/pctlauthRegisterPasscode/pctlInitialize
    // inside pctl_ops.c — that bracket is required on some firmware versions).
    // Synchronous: blocks until the applet returns.
    //
    // The notify() is deferred to the next mainLoop tick via brls::sync —
    // calling it immediately on applet return raced with the focus-state
    // event and the toast was getting silently dropped (observed on fw 22.1.0).
    pctl_logf("MAIN ACTIVITY: binding set-PIN action");
    this->item_set_pin->registerClickAction([this](brls::View*) {
        pctl_logf("UI ACTION: set-PIN clicked");
        Result rc = pctl_set_pin();
        brls::Logger::info("pctl_set_pin returned 0x{:08X}", (unsigned)rc);
        this->status_panel->refresh();
        if (R_SUCCEEDED(rc)) {
            brls::sync([]() {
                brls::Application::notify("nx_pctl/toast/set_pin_ok"_i18n);
            });
        } else {
            brls::sync([rc]() {
                brls::Application::notify(fmt::format(
                    "{} (error 0x{:08X}).",
                    "nx_pctl/toast/set_pin_err"_i18n, (unsigned)rc));
            });
        }
        return true;
    });

    // Seed the network clock from the system clock (issue #2: without a valid
    // network clock the play timer never counts down). If it is already set,
    // ask first: a hand-set system time would shift the play timer's days.
    pctl_logf("MAIN ACTIVITY: binding network-clock action");
    this->item_clock->registerClickAction([this](brls::View*) {
        pctl_logf("UI ACTION: network-clock clicked");
        ClockStatus cs;
        clock_status(&cs);
        if (!cs.network_ok) {
            this->seed_network_clock();
            return true;
        }
        auto* dialog = new brls::Dialog("nx_pctl/dialog/clock_overwrite/body"_i18n);
        dialog->addButton("hints/cancel"_i18n, []() {});
        dialog->addButton("nx_pctl/dialog/clock_overwrite/confirm"_i18n, [this]() {
            this->seed_network_clock();
        });
        dialog->open();
        return true;
    });

    // Read-only diagnostic report → SD card (attach it to bug reports).
    pctl_logf("MAIN ACTIVITY: binding diagnostic dump action");
    this->item_diag->registerClickAction([](brls::View*) {
        pctl_logf("UI ACTION: diagnostic dump clicked");
        static char buf[8192];
        pctl_logf("DIAGNOSTICS: generating play-timer dump");
        pctl_play_timer_dump(buf, sizeof(buf));
        pctl_logf("DIAGNOSTICS: play-timer dump generated");
        FILE* f = std::fopen("/switch/nx_pctl_dump.txt", "w");
        bool ok = f && std::fputs(buf, f) >= 0;
        if (f) std::fclose(f);
        brls::Application::notify(ok ? "nx_pctl/toast/diag_saved"_i18n : "nx_pctl/toast/diag_err"_i18n);
        return true;
    });

    // First read of the status panel — onResume runs it again on every entry,
    // but a freshly created Activity doesn't get an onResume call.
    pctl_logf("Refreshing main activity status panel");
    this->status_panel->refresh();
    pctl_logf("Main activity status panel refresh completed");

    pctl_logf("MAIN ACTIVITY: scheduling network-clock check");
    brls::sync([this]() {
        pctl_logf("MAIN ACTIVITY: deferred network-clock check entered");
        this->check_network_clock();
        pctl_logf("MAIN ACTIVITY: deferred network-clock check returned");
    });
    pctl_logf("MAIN ACTIVITY: onContentAvailable completed");
}

void MainActivity::check_network_clock()
{
    pctl_logf("Network clock check started");
    if (!pctl_fw_play_timer_supported()) {
        pctl_logf("Network clock check skipped: unsupported firmware");
        return;
    }
    ClockStatus cs;
    clock_status(&cs);
    pctl_logf("Network clock status: user_ok=%d network_ok=%d", cs.user_ok, cs.network_ok);
    if (cs.network_ok || !cs.user_ok) {
        pctl_logf("Network clock check completed without prompt");
        return;
    }

    auto* dialog = new brls::Dialog("nx_pctl/dialog/clock/body"_i18n);
    dialog->addButton("nx_pctl/dialog/clock/later"_i18n, []() {});
    dialog->addButton("nx_pctl/dialog/clock/confirm"_i18n, [this]() { this->seed_network_clock(); });
    dialog->open();
    pctl_logf("Network clock prompt opened");
}

void MainActivity::seed_network_clock()
{
    pctl_logf("NETWORK CLOCK: seed requested");
    Result rc = clock_seed_network_from_user();
    pctl_logf("NETWORK CLOCK: seed call returned rc=0x%08X", (unsigned)rc);
    this->status_panel->refresh();
    if (R_SUCCEEDED(rc))
        brls::Application::notify("nx_pctl/toast/clock_ok"_i18n);
    else
        brls::Application::notify(trf("nx_pctl/toast/clock_err"_i18n, (unsigned)rc));
}

void MainActivity::onResume()
{
    pctl_logf("Main activity resumed; refreshing status panel");
    brls::Activity::onResume();
    this->status_panel->refresh();
    pctl_logf("Main activity resume refresh completed");
}
