// Central-server launch gate. The console PIN remains managed by Nintendo's system UI.
// Copyright (C) 2026 Taylor. GPLv3-or-later (see LICENSE).
#include "activity/pin_gate_activity.hpp"

#include <string>

#include "activity/main_activity.hpp"
#include "remote_control.hpp"
#include "util/pctl_ops_c.hpp"

using namespace brls::literals;

void PinGateActivity::onContentAvailable()
{
    pctl_logf("PIN GATE: onContentAvailable entered");
    this->getContentView()->registerAction(
        "nx_pctl/pin_gate/enter"_i18n, brls::BUTTON_A,
        [this](brls::View*) {
            this->authenticate();
            return true;
        });
    this->getContentView()->registerAction(
        "hints/exit"_i18n, brls::BUTTON_B,
        [](brls::View*) {
            brls::Application::quit();
            return true;
        });

    // Run after the Activity's XML bindings are available.
    pctl_logf("PIN GATE: registering deferred auto-auth callback");
    brls::sync([this]() {
        pctl_logf("PIN GATE: deferred auto-auth callback entered");
        this->authenticate();
        pctl_logf("PIN GATE: deferred auto-auth callback returned");
    });
    pctl_logf("PIN GATE: onContentAvailable returning");
}

void PinGateActivity::onResume()
{
    pctl_logf("PIN GATE: onResume entered");
    brls::Activity::onResume();
    pctl_logf("PIN GATE: base onResume returned");
}

void PinGateActivity::authenticate()
{
    pctl_logf("AUTH UI: authenticate entered checking=%d passed=%d", this->checking, this->passed);
    if (this->checking) {
        pctl_logf("AUTH UI: skipped because authentication is already in progress");
        return;
    }
    this->checking = true;
    pctl_logf("AUTH UI: setting status label to contacting");
    this->status->setText("Contacting central control server…");
    pctl_logf("AUTH UI: status label updated; calling remote_control::authenticate");

    std::string message;
    const bool ok = remote_control::authenticate(message);
    pctl_logf("AUTH UI: remote_control::authenticate returned ok=%d message_len=%u",
              ok, (unsigned)message.size());
    this->checking = false;
    pctl_logf("AUTH UI: updating status label with authentication result");
    this->status->setText(message);
    pctl_logf("AUTH UI: status label updated");

    if (!ok) {
        pctl_logf("Central-control authentication denied; reason: %s", message.c_str());
        return;
    }

    pctl_logf("Central-control authentication succeeded");
    if (this->passed) return;
    this->passed = true;
    pctl_logf("AUTH UI: constructing MainActivity");
    MainActivity* main_activity = new MainActivity();
    pctl_logf("AUTH UI: MainActivity constructed; calling pushActivity(NONE)");
    brls::Application::pushActivity(main_activity, brls::TransitionAnimation::NONE);
    pctl_logf("AUTH UI: pushActivity(MainActivity) returned");
}
