// NX-Pctl-Manager — thin wrappers around the system parental-control (`pctl`)
// service. All user-facing strings live in the UI layer; this layer is data only.
// Copyright (C) 2026 Taylor.  This program is free software under the GNU
// General Public License v3 or later; it comes with NO WARRANTY. See the
// LICENSE file or <https://www.gnu.org/licenses/gpl-3.0.html> for details.
#pragma once
#include <switch.h>

typedef enum {
    PctlSafetyLevel_None       = 0,
    PctlSafetyLevel_Custom     = 1,
    PctlSafetyLevel_YoungChild = 2,
    PctlSafetyLevel_Child      = 3,
    PctlSafetyLevel_Teen       = 4,
} PctlSafetyLevel;

// Best-effort snapshot of the current parental-control state. A field's `*_ok`
// flag is false when that particular query failed (e.g. on a firmware where the
// command is unavailable) — callers should show "unavailable" rather than 0.
typedef struct {
    bool safety_level_ok;
    u32  safety_level;            // a PctlSafetyLevel value when safety_level_ok

    bool pin_length_ok;
    u32  pin_length;              // number of PIN digits; 0 == no PIN registered

    bool restriction_enabled_ok;
    bool restriction_enabled;
} PctlStatus;

Result pctl_ops_init(void);   // open the `pctl` service (needs CFW for the privileged calls)
void   pctl_ops_exit(void);
Result pctl_ops_reinit(void); // close + reopen the session (recovers if a bad command closed it)

// The play-timer commands this tool uses (145601 / 195101 with the 0x44-byte
// PlayTimerSettings) only exist in that shape on fw 21.0.0+. On older firmware
// every play-timer read/write is refused instead of sending a mis-sized struct.
bool pctl_fw_play_timer_supported(void);

void pctl_status_fetch(PctlStatus *out);

Result pctl_set_pin(void);                   // opens the OS PIN screen (registers or changes the PIN)
Result pctl_delete_parental_controls(void);  // wipes the PIN and every restriction — CANNOT BE UNDONE
Result pctl_delete_pairing(void);            // unlinks the companion mobile app from this console

// GetPinCode (cmd 1208): copies the PIN digits (NUL-terminated) into out and
// returns their count in *len (0 == no PIN). Used by the temporary unlock and
// by the launch-time PIN gate.
Result pctl_get_pin(char *out, size_t outsz, u32 *len);

// UnlockRestrictionTemporarily (cmd 1201): temporarily lifts the parental-control
// restriction (and, on fw 22.1.0, makes IsPlayTimerEnabled read false). Reads the
// current PIN via GetPinCode (cmd 1208) and passes it back. The PIN goes in a
// HIPC *pointer* buffer, NUL-terminated. Fails with MAKERESULT(Module_Libnx,
// LibnxError_NotFound) without calling 1201 when no PIN is registered.
Result pctl_unlock_restriction_temporarily(void);

// RevertRestrictionTemporaryUnlocked (cmd 1007): ends a temporary unlock — the
// same thing as re-enabling parental controls from the HOME-menu banner.
Result pctl_revert_temporary_unlock(void);

// IsRestrictionTemporaryUnlocked (cmd 1006).
bool pctl_is_temporarily_unlocked(void);

const char *pctl_safety_level_name(u32 level);

// ---- play timer ----

// nn::pctl::PlayTimerSettings, fw 21.0.0+ (0x44 bytes). Layout reconstructed
// from (a) a dump of a console configured through the companion app and (b)
// sizes: the pre-21 struct is 0x34 == 4 + 8 * 6 (day settings without the two
// bedtime-reset bytes, which arrived with the fw 20 GetBedtimeAlarmResetTime*
// commands), the new one 0x44 == 4 + 8 * 8. Fields marked "?" are unconfirmed.
typedef struct {
    u8  bedtime_enabled;        // Bedtime alarm on for this day
    u8  bedtime_hour;           // alarm time
    u8  bedtime_minute;
    u8  bedtime_reset_hour;     // restriction lifts at (companion-app default 06:00)
    u8  bedtime_reset_minute;
    u8  limit_enabled;          // this day has a play-time limit
    u16 limit_minutes;          // 0 == fully blocked that day
} PtDaySetting;

typedef struct {
    u8  enabled;                // non-zero == IsPlayTimerEnabled
    u8  hdr1;                   // ? 1 in the companion-app dump (restriction mode?)
    u8  hdr2;                   // ? 1 in the companion-app dump (per-day mode?)
    u8  hdr3;                   // 0
    PtDaySetting daily;         // "same every day" slot — all-zero in the dump
    PtDaySetting weekly[7];     // Sun..Sat
} PtSettings;

_Static_assert(sizeof(PtDaySetting) == 8,    "PtDaySetting must be 8 bytes");
_Static_assert(sizeof(PtSettings)   == 0x44, "PtSettings must be 0x44 bytes");

// Writes a multi-line read-only diagnostic report into buf (firmware, clocks,
// play-timer commands, raw + decoded PlayTimerSettings, bedtime getters,
// restriction / PIN state with the digits masked). Touches nothing.
void pctl_play_timer_dump(char *buf, size_t bufsz);

// Per-day value sentinel: this day has no configured limit at all (unrestricted).
// (0 means a *0-minute* limit — i.e. that day is fully blocked. They are different.)
#define PT_DAY_NOLIMIT 0xFFFFu

typedef struct {
    bool enabled;
    u8   hour, minute;              // alarm time
    u8   reset_hour, reset_minute;  // restriction lifts at
} PtBedtime;

// Snapshot of the play-timer state.
typedef struct {
    bool supported;      // firmware has the 0x44-byte play-timer commands
    bool valid;          // GetPlayTimerSettings (145601) succeeded
    bool enabled;        // IsPlayTimerEnabled (1453)
    bool restricted;     // IsRestrictedByPlayTimer (1455) — true == today's limit reached
    u64  remaining_ns;   // GetPlayTimerRemainingTime (1454), ns; often 0 with no game running
    u16  day_min[7];     // per-day, Sun..Sat: minutes (0 == no play that day), or PT_DAY_NOLIMIT == no limit
    PtBedtime bedtime;   // decoded from Sunday's slot (this tool writes the same bedtime to every day)
    PtSettings raw;      // the settings exactly as read
} PtState;

void pctl_play_timer_query(PtState *out);

// Sets per-day play-time limits (days_min[0]=Sunday .. [6]=Saturday): each is minutes
// (0 == that day is fully blocked), or PT_DAY_NOLIMIT to leave that day unrestricted.
// The current bedtime alarm is preserved. If no day has a limit and the bedtime
// alarm is off, the play timer is turned off entirely.
Result pctl_play_timer_set_days(const u16 days_min[7]);

// Convenience: the same minute limit on every day (minutes == 0 blocks every day —
// to turn the timer OFF instead, use pctl_play_timer_clear).
Result pctl_play_timer_set_uniform(u16 minutes);

// Removes every daily limit (the bedtime alarm, if any, is kept).
Result pctl_play_timer_clear(void);

// Sets the bedtime alarm on every day (limits preserved). Pass enabled=false to
// turn it off.
Result pctl_play_timer_set_bedtime(const PtBedtime *bed);

// Reads the system's own view of the bedtime alarm back via 1954 / 1956-1959,
// to confirm a write was interpreted as intended. *ok is false if any getter failed.
void pctl_bedtime_readback(PtBedtime *out, bool *ok);

// PROBE builds only: writes the settings with header bytes 1 / 2 overridden
// (everything else as currently configured) — used to pin down what they mean.
Result pctl_play_timer_write_raw(const PtSettings *s);

// Appends one line to sd:/switch/nx_pctl_manager.log (best effort).
void pctl_logf(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
