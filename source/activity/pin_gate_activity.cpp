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
    brls::sync([this]() { this->authenticate(); });
}

void PinGateActivity::onResume()
{
    brls::Activity::onResume();
}

void PinGateActivity::authenticate()
{
    if (this->checking) return;
    this->checking = true;
    this->status->setText("Contacting central control server…");

    std::string message;
    const bool ok = remote_control::authenticate(message);
    this->checking = false;
    this->status->setText(message);

    if (!ok) {
        pctl_logf("Central-control authentication denied; reason: %s", message.c_str());
        return;
    }

    pctl_logf("Central-control authentication succeeded");
    if (this->passed) return;
    this->passed = true;
    pctl_logf("Opening main activity after central-control authentication");
    brls::Application::pushActivity(
        new MainActivity(), brls::TransitionAnimation::NONE);
    pctl_logf("Main activity push completed");
}
