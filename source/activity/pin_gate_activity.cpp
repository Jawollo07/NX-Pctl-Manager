// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#include "activity/pin_gate_activity.hpp"

#include <cstring>

#include "activity/main_activity.hpp"
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

    // Open the keypad straight away; deferred so the first frame is drawn first.
    brls::sync([this]() { this->prompt(); });
}

void PinGateActivity::onResume()
{
    brls::Activity::onResume();
    // Backing out of the main menu lands here — that's "exit", not "lock again".
    if (this->passed) brls::Application::quit();
}

void PinGateActivity::prompt()
{
    if (this->passed) return;

    // Read the PIN before opening the keypad. Start-up also sends us here when
    // 1206 GetPinCodeLength fails, so the console may have no PIN at all:
    // 1208 succeeding with len 0 confirms that. A failed 1208 keeps the gate shut.
    char pin[32];
    u32 len = 0;
    Result rc = pctl_get_pin(pin, sizeof(pin), &len);
    if (R_FAILED(rc)) {
        this->status->setText(trf("nx_pctl/pin_gate/read_failed"_i18n, (unsigned)rc));
        return;
    }
    if (len == 0) {
        pctl_logf("PIN gate: 1208 reports no PIN registered, skipping the gate");
        this->passed = true;
        brls::Application::pushActivity(new MainActivity());
        return;
    }

    auto entered = numpad::prompt_pin("nx_pctl/pin_gate/title"_i18n);
    bool ok = entered.has_value() && *entered == std::string(pin, len);
    std::memset(pin, 0, sizeof(pin));
    if (!entered.has_value()) return;   // cancelled — stay on the gate (A retries, B exits)

    if (ok) {
        this->passed = true;
        brls::Application::pushActivity(new MainActivity());
        return;
    }

    this->attempts++;
    pctl_logf("PIN gate: wrong PIN (attempt %d/%d)", this->attempts, MAX_ATTEMPTS);
    if (this->attempts >= MAX_ATTEMPTS) {
        brls::Application::quit();
        return;
    }
    this->status->setText(trf("nx_pctl/pin_gate/wrong"_i18n, MAX_ATTEMPTS - this->attempts));
}
