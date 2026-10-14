// pt_flow — the one path every play-timer write goes through.
//
// Writing a new config while the play timer is active can lock the console
// and destabilise Atmosphère, so the write is bracketed:
//   1. refuse on firmware without the 0x44-byte commands, or with no PIN
//      registered (the temporary unlock needs one);
//   2. if the timer is active, cmd 1201 UnlockRestrictionTemporarily;
//   3. write;
//   4. cmd 1007 RevertRestrictionTemporaryUnlocked — v3.0.0 skipped this and
//      left parental controls switched off until the user re-enabled them from
//      the HOME menu, which is why limits "did nothing" (issues #3 / #6 / #7).
// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#pragma once

#include <functional>
#include <string>

#include "util/pctl_ops_c.hpp"

namespace pt_flow
{

// Runs `write` through the steps above and reports the outcome to the user:
// `ok_text` as a toast on success, otherwise an error toast / dialog. Returns
// true when the write itself succeeded.
bool guarded_write(const std::function<Result()>& write, const std::string& ok_text);

}   // namespace pt_flow
