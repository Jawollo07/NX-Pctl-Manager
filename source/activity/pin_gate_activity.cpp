// Copyright (C) 2026 Taylor. GPLv3-or-later (see LICENSE).
#include "activity/pin_gate_activity.hpp"

#include <array>
#include <cstring>
#include <string>

#include "activity/main_activity.hpp"
#include "util/numpad.hpp"
#include "util/pctl_ops_c.hpp"
#include "util/totp.hpp"
#include "util/trf.hpp"

using namespace brls::literals;

namespace
{
constexpr int MAX_ATTEMPTS = 5;

void wipe_secret(std::array<uint8_t, totp::SECRET_BYTES>& secret)
{
    volatile uint8_t* p = secret.data();
    for (std::size_t i = 0; i < secret.size(); ++i) p[i] = 0;
}
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
    if (this->setup_pending) {
        this->passed = true;
        brls::Application::pushActivity(new MainActivity());
        return;
    }

    // First-run enrollment: authenticate with the console PIN, then show a
    // one-time TOTP secret for manual entry into a compatible authenticator.
    if (!totp::secret_file_exists()) {
        char pin[32] = {};
        u32 len = 0;
        Result rc = pctl_get_pin(pin, sizeof(pin), &len);
        if (R_FAILED(rc) || len == 0) {
            std::memset(pin, 0, sizeof(pin));
            this->status->setText("Cannot enroll: set a console parental-control PIN first.");
            return;
        }

        auto entered = numpad::prompt_pin("Enter console PIN to enroll dynamic parent code");
        const bool ok = entered.has_value() && *entered == std::string(pin, len);
        std::memset(pin, 0, sizeof(pin));
        if (!entered.has_value()) return;
        if (!ok) {
            this->attempts++;
            pctl_logf("TOTP enrollment: wrong console PIN (attempt %d/%d)", this->attempts, MAX_ATTEMPTS);
            if (this->attempts >= MAX_ATTEMPTS) { brls::Application::quit(); return; }
            this->status->setText(trf("nx_pctl/pin_gate/wrong"_i18n, MAX_ATTEMPTS - this->attempts));
            return;
        }

        std::array<uint8_t, totp::SECRET_BYTES> secret{};
        if (!totp::load_or_create_secret(secret)) {
            wipe_secret(secret);
            this->status->setText("Could not create the TOTP secret on the SD card. Check SD card space and write access.");
            return;
        }
        const std::string encoded = totp::base32(secret.data(), secret.size());
        wipe_secret(secret);
        pctl_logf("TOTP parent-code enrollment completed; secret is not written to logs");
        this->setup_pending = true;
        this->status->setText(
            "ONE-TIME SETUP — add an account in your authenticator app using this secret:\n\n" +
            encoded +
            "\n\nChoose TOTP / time-based, SHA-1, 6 digits, period 60 seconds. "
            "Keep this secret private. Press A only after saving it; it will not be shown again.");
        return;
    }

    auto entered = numpad::prompt_pin("Enter the current 6-digit parent code");
    if (!entered.has_value()) return;
    if (totp::verify_code(*entered)) {
        this->passed = true;
        brls::Application::pushActivity(new MainActivity());
        return;
    }

    this->attempts++;
    pctl_logf("TOTP gate: invalid code (attempt %d/%d)", this->attempts, MAX_ATTEMPTS);
    if (this->attempts >= MAX_ATTEMPTS) {
        brls::Application::quit();
        return;
    }
    this->status->setText(trf("nx_pctl/pin_gate/wrong"_i18n, MAX_ATTEMPTS - this->attempts));
}
