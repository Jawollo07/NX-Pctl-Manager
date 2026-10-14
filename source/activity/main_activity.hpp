// MainActivity — root view. Hosts the StatusPanel (read from pctl_ops) and
// the action cells. Layout in resources/xml/activity/main.xml; per-cell
// wiring in onContentAvailable.
// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#pragma once

#include <borealis.hpp>

#include "view/status_panel.hpp"

class MainActivity : public brls::Activity
{
  public:
    CONTENT_FROM_XML_RES("activity/main.xml");

    void onContentAvailable() override;
    void onResume() override;

  private:
    BRLS_BIND(StatusPanel,       status_panel,    "status_panel");
    BRLS_BIND(brls::DetailCell,  item_refresh,    "item_refresh");
    BRLS_BIND(brls::DetailCell,  item_play_timer, "item_play_timer");
    BRLS_BIND(brls::DetailCell,  item_set_pin,    "item_set_pin");
    BRLS_BIND(brls::DetailCell,  item_delete_pc,  "item_delete_pc");
    BRLS_BIND(brls::DetailCell,  item_unlink,     "item_unlink");
    BRLS_BIND(brls::DetailCell,  item_clock,      "item_clock");
    BRLS_BIND(brls::DetailCell,  item_diag,       "item_diag");

    void check_network_clock();   // one-time prompt when the network clock was never set
};
