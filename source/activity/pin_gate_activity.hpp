// Launch gate: central-server authentication.
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
    void authenticate();

    bool checking = false;

    bool passed = false;

    BRLS_BIND(brls::Label, status, "status");
};
