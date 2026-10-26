// PinGateActivity — root Activity for the console PIN enrollment and TOTP gate.
// Wrong codes 5 times, or B, exits. The TOTP secret is stored on SD and is
// not protected from someone who can read the SD card or run homebrew.
// Copyright (C) 2026 Taylor. GPLv3-or-later (see LICENSE).
#pragma once

#include <borealis.hpp>

class PinGateActivity : public brls::Activity
{
  public:
    CONTENT_FROM_XML_RES("activity/pin_gate.xml");

    void onContentAvailable() override;
    void onResume() override;

  private:
    void prompt();

    int  attempts = 0;
    bool passed = false;
    bool setup_pending = false;

    BRLS_BIND(brls::Label, status, "status");
};
