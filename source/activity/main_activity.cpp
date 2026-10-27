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
#include "util/totp.hpp"
#include "util/trf.hpp"

using namespace brls::literals;   // for "key"_i18n

namespace
{
void wipe_totp_secret(std::array<uint8_t, totp::SECRET_BYTES>& secret)
{
    volatile uint8_t* p = secret.data();
    for (std::size_t i = 0; i < secret.size(); ++i) p[i] = 0;
}

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
    // Refresh status — step (c): re-read pctl state and update the panel.
    // The status panel already refreshes on onResume; this lets the user force
    // a re-read without leaving the screen.
    this->item_refresh->registerClickAction([this](brls::View*) {
        this->status_panel->refresh();
        brls::Application::notify("nx_pctl/toast/status_refreshed"_i18n);
        return true;
    });

    // Rotate the dynamic parent code. Require the real console PIN again before
    // invalidating the old authenticator secret.
    this->item_totp->registerClickAction([this](brls::View*) {
        if (!totp::secret_file_exists()) {
            brls::Application::notify("nx_pctl/totp/not_configured"_i18n);
            return true;
        }

        char pin[32] = {};
        u32 len = 0;
        Result rc = pctl_get_pin(pin, sizeof(pin), &len);
        if (R_FAILED(rc) || len == 0 || len > sizeof(pin)) {
            std::memset(pin, 0, sizeof(pin));
            brls::Application::notify("nx_pctl/totp/pin_read_failed"_i18n);
            return true;
        }

        auto entered = numpad::prompt_pin("Re-enter console parental-control PIN to rotate parent code");
        const bool ok = entered.has_value() && *entered == std::string(pin, len);
        std::memset(pin, 0, sizeof(pin));
        if (!entered.has_value()) return true;
        if (!ok) {
            brls::Application::notify("nx_pctl/totp/wrong_pin"_i18n);
            return true;
        }

        std::array<uint8_t, totp::SECRET_BYTES> secret{};
        if (!totp::rotate_secret(secret)) {
            wipe_totp_secret(secret);
            brls::Application::notify("nx_pctl/totp/rotate_failed"_i18n);
            return true;
        }
        const std::string encoded = totp::base32(secret.data(), secret.size());
        wipe_totp_secret(secret);

        auto* dialog = new brls::Dialog(
            "nx_pctl/totp/new_secret_body"_i18n + "\n\n" + encoded +
            "\n\n" + "nx_pctl/totp/new_secret_footer"_i18n);
        dialog->addButton("nx_pctl/totp/ack"_i18n, []() {});
        dialog->open();
        return true;
    });

    // Delete all parental controls (cmd 1043) — step (d), destructive, confirm-gated.
    this->item_delete_pc->registerClickAction([this](brls::View*) {
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
    this->item_unlink->registerClickAction([this](brls::View*) {
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
    this->item_play_timer->registerClickAction([](brls::View*) {
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
    this->item_set_pin->registerClickAction([this](brls::View*) {
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
    this->item_clock->registerClickAction([this](brls::View*) {
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
    this->item_diag->registerClickAction([](brls::View*) {
        static char buf[8192];
        pctl_play_timer_dump(buf, sizeof(buf));
        FILE* f = std::fopen("/switch/nx_pctl_dump.txt", "w");
        bool ok = f && std::fputs(buf, f) >= 0;
        if (f) std::fclose(f);
        brls::Application::notify(ok ? "nx_pctl/toast/diag_saved"_i18n : "nx_pctl/toast/diag_err"_i18n);
        return true;
    });

    // First read of the status panel — onResume runs it again on every entry,
    // but a freshly created Activity doesn't get an onResume call.
    this->status_panel->refresh();

    brls::sync([this]() { this->check_network_clock(); });
}

void MainActivity::check_network_clock()
{
    if (!pctl_fw_play_timer_supported()) return;
    ClockStatus cs;
    clock_status(&cs);
    if (cs.network_ok || !cs.user_ok) return;

    auto* dialog = new brls::Dialog("nx_pctl/dialog/clock/body"_i18n);
    dialog->addButton("nx_pctl/dialog/clock/later"_i18n, []() {});
    dialog->addButton("nx_pctl/dialog/clock/confirm"_i18n, [this]() { this->seed_network_clock(); });
    dialog->open();
}

void MainActivity::seed_network_clock()
{
    Result rc = clock_seed_network_from_user();
    this->status_panel->refresh();
    if (R_SUCCEEDED(rc))
        brls::Application::notify("nx_pctl/toast/clock_ok"_i18n);
    else
        brls::Application::notify(trf("nx_pctl/toast/clock_err"_i18n, (unsigned)rc));
}

void MainActivity::onResume()
{
    brls::Activity::onResume();
    this->status_panel->refresh();
}
