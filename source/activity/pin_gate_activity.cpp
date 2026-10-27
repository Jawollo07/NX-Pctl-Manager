// Central-server launch gate; the Nintendo console PIN remains local.
// Copyright (C) 2026 Taylor. GPLv3-or-later (see LICENSE).
#include "activity/pin_gate_activity.hpp"

#include <cstring>
#include <string>

#include "activity/main_activity.hpp"
#include "remote_control.hpp"
#include "util/numpad.hpp"
#include "util/pctl_ops_c.hpp"
#include "util/trf.hpp"

using namespace brls::literals;

namespace
{
constexpr int MAX_ATTEMPTS = 5;
}

void PinGateActivity::onContentAvailable()
{
    this->getContentView()->registerAction(
        "nx_pctl/pin_gate/enter"_i18n, brls::BUTTON_A,
        [this](brls::View*) {
            this->prompt();
            return true;
        });
    this->getContentView()->registerAction(
        "hints/exit"_i18n, brls::BUTTON_B,
        [](brls::View*) {
            brls::Application::quit();
            return true;
        });

    brls::sync([this]() { this->prompt(); });
}

void PinGateActivity::onResume()
{
    brls::Activity::onResume();
    if (this->passed) brls::Application::quit();
}

void PinGateActivity::prompt()
{
    if (this->passed) return;

    // Keep the console's actual parental-control PIN as a local check when one
    // is registered. The PIN is never sent to the central server.
    PctlStatus state{};
    pctl_status_fetch(&state);
    if (state.pin_length_ok && state.pin_length > 0) {
        char pin[32] = {};
        u32 len = 0;
        const Result rc = pctl_get_pin(pin, sizeof(pin), &len);
        if (R_FAILED(rc) || len == 0 || len > sizeof(pin)) {
            std::memset(pin, 0, sizeof(pin));
            this->status->setText("Cannot read the console PIN. Check parental-control setup.");
            return;
        }

        auto entered = numpad::prompt_pin("Enter the console parental-control PIN");
        const bool ok = entered.has_value() && *entered == std::string(pin, len);
        std::memset(pin, 0, sizeof(pin));
        if (!entered.has_value()) return;
        if (!ok) {
            this->attempts++;
            pctl_logf("Central-control gate: wrong local console PIN (attempt %d/%d)",
                      this->attempts, MAX_ATTEMPTS);
            if (this->attempts >= MAX_ATTEMPTS) {
                brls::Application::quit();
                return;
            }
            this->status->setText(trf("nx_pctl/pin_gate/wrong"_i18n,
                                      MAX_ATTEMPTS - this->attempts));
            return;
        }
    }

    // This is the replacement for TOTP: the server validates the per-device
    // bearer credential over verified HTTPS. Fail closed when unavailable.
    std::string message;
    if (!remote_control::authenticate(message)) {
        pctl_logf("Central-control authentication denied; reason: %s", message.c_str());
        this->status->setText(message);
        return;
    }

    pctl_logf("Central-control authentication succeeded");
    this->passed = true;
    brls::Application::pushActivity(new MainActivity());
}
