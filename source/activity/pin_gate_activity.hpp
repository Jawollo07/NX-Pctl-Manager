// PinGateActivity — root Activity when a parental-control PIN is registered.
// This tool can remove every restriction, so it asks for the console's own
// PIN before showing the main menu. Wrong PIN 5 times, or B, exits the app.
// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
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
    bool passed   = false;   // MainActivity was pushed on top of us

    BRLS_BIND(brls::Label, status, "status");
};
