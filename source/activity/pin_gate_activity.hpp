// Launch gate: optional local console PIN check followed by central-server auth.
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

    int attempts = 0;
    bool passed = false;

    BRLS_BIND(brls::Label, status, "status");
};
