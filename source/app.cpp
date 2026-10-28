// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#include "app.hpp"
#include "util/pctl_ops_c.hpp"

namespace app
{

static bool     s_ok          = false;
static uint32_t s_init_result = 0;

bool init()
{
    pctl_logf("APP INIT: calling pctl_ops_init()");
    Result rc = pctl_ops_init();
    s_init_result = (uint32_t)rc;
    s_ok = R_SUCCEEDED(rc);
    pctl_logf("APP INIT: pctl_ops_init rc=0x%08X success=%d", (unsigned)rc, s_ok);
    return s_ok;
}

void shutdown()
{
    pctl_logf("APP SHUTDOWN: entered; service_open=%d", s_ok);
    if (s_ok) {
        pctl_logf("APP SHUTDOWN: calling pctl_ops_exit()");
        pctl_ops_exit();
        pctl_logf("APP SHUTDOWN: pctl_ops_exit returned");
    }
    s_ok = false;
    pctl_logf("APP SHUTDOWN: completed");
}

bool     pctl_available()  { return s_ok; }
uint32_t pctl_init_result(){ return s_init_result; }

}   // namespace app
