// NX-Pctl-Manager — pctl service layer (see pctl_ops.h).
// Copyright (C) 2026 Taylor.  This program is free software under the GNU
// General Public License v3 or later; it comes with NO WARRANTY. See the
// LICENSE file or <https://www.gnu.org/licenses/gpl-3.0.html> for details.
#include "pctl_ops.h"
#include <string.h>
#include <stdio.h>
#include <stdarg.h>
#include <time.h>

/*
 * Command IDs for IParentalControlService.
 * Reference: https://switchbrew.org/wiki/Parental_Control_services
 *
 * If one of these starts returning errors after a system update, re-check the
 * wiki — Nintendo has shifted command IDs across firmware versions before.
 *
 *   1006 IsRestrictionTemporaryUnlocked  -> bool   (read-only; goes true after a successful 1201)
 *   1007 RevertRestrictionTemporaryUnlocked (no args) -- ends a 1201 unlock (what re-enabling parental
 *                                       controls from the HOME-menu banner does)
 *   1031 IsRestrictionEnabled    -> bool   (read-only)
 *   1032 GetSafetyLevel          -> u32    (read-only)
 *   1043 DeleteSettings          (no args) (privileged: pctl:s/pctl:a) -- IRREVERSIBLE
 *   1201 UnlockRestrictionTemporarily  (in: the PIN as an In|HipcPointer buffer ["type 0x9"], NUL-terminated
 *                                       (digits + '\0' == GetPinCodeLength()+1 bytes); no out) -- verified on
 *                                       fw 22.1.0. A bare no-buffer call, or one sent as MapAlias instead of
 *                                       HipcPointer, returns 0xF601 (ResultSessionClosed: the sysmodule drops
 *                                       the session); a buffer of just the digits (no '\0') returns 0xF80E.
 *   1206 GetPinCodeLength        -> u32    (read-only; 0 == no PIN registered)
 *   1208 GetPinCode              -> u32 len + the PIN digits in an Out|HipcPointer buffer ["type 0xA"]  [4.0.0+]
 *   1941 DeletePairing           (no args) (privileged)
 *   (1601/1602/1603 DisableAllFeatures/PostEnableAllFeatures/IsAllFeaturesDisabled all return pctl error
 *    0x00010A8E [module 142 / desc 133] on a normal console — not usable.)
 *   --- play timer (details further down, near pctl_play_timer_query / _dump) ---
 *   1451 StartPlayTimer / 1452 StopPlayTimer  (no args; control the *running* countdown)
 *   1453 IsPlayTimerEnabled -> bool   1454 GetPlayTimerRemainingTime   1455 IsRestrictedByPlayTimer -> bool
 *   145601 GetPlayTimerSettings -> PlayTimerSettings   195101 SetPlayTimerSettingsForDebug <- PlayTimerSettings
 *          (both 18.0.0+, 0x44 bytes only on 21.0.0+ -- see pctl_fw_play_timer_supported)
 *   1952 GetPlayTimerSpentTimeForTest   1954 IsBedtimeAlarmEnabled -> bool
 *   1956/1957 GetBedtimeAlarmTimeHour/Minute -> u8   1958/1959 GetBedtimeAlarmResetTimeHour/Minute -> u8 [20.0.0+]
 *   (Never call 1456 / 1951 on fw 22 — they close the pctl session.)
 *
 * Registering/changing the PIN goes through the OS library applet
 * (pctlauthRegisterPasscode), not the service: the direct SetPinCode command is
 * rejected on current firmware.
 *
 * The privileged calls only succeed on a console running CFW (Atmosphere), where
 * libnx's auto-init lands on a `pctl:a`/`pctl:s` session; on a stock console
 * pctl_ops_init() itself fails and the app never gets this far.
 */

Result pctl_ops_init(void) { return pctlInitialize(); }
void   pctl_ops_exit(void) { pctlExit(); }

bool pctl_fw_play_timer_supported(void) { return hosversionAtLeast(21, 0, 0); }

void pctl_logf(const char *fmt, ...)
{
    FILE *f = fopen("/switch/nx_pctl_manager.log", "a");
    if (!f) return;
    time_t now = time(NULL);
    struct tm *t = localtime(&now);
    if (t) fprintf(f, "%04d-%02d-%02d %02d:%02d:%02d  ", t->tm_year + 1900, t->tm_mon + 1,
                   t->tm_mday, t->tm_hour, t->tm_min, t->tm_sec);
    va_list ap;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

Result pctl_ops_reinit(void)
{
    // main() holds exactly one pctl reference, so one exit drops the refcount to
    // zero and actually closes the (possibly dead) session; init reopens a fresh one.
    pctlExit();
    return pctlInitialize();
}

void pctl_status_fetch(PctlStatus *out)
{
    memset(out, 0, sizeof(*out));
    Service *srv = pctlGetServiceSession_Service();

    u32 level = 0;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1032, level))) {
        out->safety_level = level;
        out->safety_level_ok = true;
    }

    u32 pin_len = 0;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1206, pin_len))) {
        out->pin_length = pin_len;
        out->pin_length_ok = true;
    }

    bool enabled = false;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1031, enabled))) {
        out->restriction_enabled = enabled;
        out->restriction_enabled_ok = true;
    }
}

Result pctl_set_pin(void)
{
    // The pctlauth applet opens its own privileged pctl session; on some firmware
    // versions that fails while our process still holds one, so drop ours around
    // the call (this exit/init dance is needed).
    pctlExit();
    Result rc = pctlauthRegisterPasscode();
    pctlInitialize();   // re-open our session; a failure here surfaces on the next op
    return rc;
}

Result pctl_delete_parental_controls(void)
{
    Result rc = serviceDispatch(pctlGetServiceSession_Service(), 1043);
    pctl_logf("1043 DeleteSettings rc=0x%08X", (unsigned)rc);
    return rc;
}

Result pctl_delete_pairing(void)
{
    Result rc = serviceDispatch(pctlGetServiceSession_Service(), 1941);
    pctl_logf("1941 DeletePairing rc=0x%08X", (unsigned)rc);
    return rc;
}

Result pctl_get_pin(char *out, size_t outsz, u32 *len)
{
    // GetPinCode (1208): out u32 (length) + out pointer buffer (digits + null-pad)
    memset(out, 0, outsz);
    *len = 0;
    Result rc = serviceDispatchOut(pctlGetServiceSession_Service(), 1208, *len,
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out },
        .buffers      = { { out, outsz } });
    if (R_FAILED(rc)) { *len = 0; return rc; }
    if (*len >= outsz) *len = (u32)outsz - 1;
    out[*len] = '\0';
    return rc;
}

Result pctl_revert_temporary_unlock(void)
{
    pctl_ops_reinit();
    Result rc = serviceDispatch(pctlGetServiceSession_Service(), 1007);
    pctl_logf("1007 RevertRestrictionTemporaryUnlocked rc=0x%08X", (unsigned)rc);
    return rc;
}

bool pctl_is_temporarily_unlocked(void)
{
    bool b = false;
    if (R_FAILED(serviceDispatchOut(pctlGetServiceSession_Service(), 1006, b))) return false;
    return b;
}

Result pctl_unlock_restriction_temporarily(void)
{
    // Temporarily lift the parental-control restriction (cmd 1201). Verified on
    // fw 22.1.0. Two things had to be right (both learned the hard way):
    //  - the buffer descriptors are HIPC *pointer* buffers ("type 0x9"/"0xA" in
    //    SupercellNx's generated pctl IPC stubs == SfBufferAttr_In|HipcPointer /
    //    Out|HipcPointer), not map-alias — sending map-alias makes the sysmodule
    //    drop the session (0xF601);
    //  - the PIN must be passed NUL-terminated (digits + '\0', i.e. GetPinCodeLength
    //    + 1 bytes) — sending just the digits returns 0xF80E.
    // We read the current PIN with GetPinCode (1208) and hand it back, so this works
    // even if the user has forgotten it (we hold a privileged pctl session under CFW).
    pctl_ops_reinit();

    char pin[32];
    u32 pin_len = 0;
    Result rc = pctl_get_pin(pin, sizeof(pin), &pin_len);
    if (R_FAILED(rc)) { pctl_logf("1208 GetPinCode rc=0x%08X", (unsigned)rc); return rc; }
    if (pin_len == 0) {
        // No PIN registered: 1201 has nothing valid to check against (v3.0.0 sent
        // 32 zero bytes here). Callers are expected to require a PIN first.
        pctl_logf("1201 skipped: no PIN registered");
        return MAKERESULT(Module_Libnx, LibnxError_NotFound);
    }

    rc = serviceDispatch(pctlGetServiceSession_Service(), 1201,            // UnlockRestrictionTemporarily <- PIN pointer buffer
        .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_In },
        .buffers      = { { pin, (size_t)pin_len + 1 } });                 // PIN + the trailing '\0'
    memset(pin, 0, sizeof(pin));
    pctl_logf("1201 UnlockRestrictionTemporarily rc=0x%08X", (unsigned)rc);
    return rc;
}

const char *pctl_safety_level_name(u32 level)
{
    switch (level) {
        case PctlSafetyLevel_None:       return "None";
        case PctlSafetyLevel_Custom:     return "Custom";
        case PctlSafetyLevel_YoungChild: return "Young Child";
        case PctlSafetyLevel_Child:      return "Child";
        case PctlSafetyLevel_Teen:       return "Teen";
        default:                         return "Unknown";
    }
}


/* --------------------------------------------------------------------------
 * Play timer — the command IDs are in the table at the top of this file.
 *
 * On fw 21+ the daily play-time limit can't be set in the on-console Settings
 * (only via Nintendo's companion app + sync); the only "set" command in `pctl` is
 * SetPlayTimerSettingsForDebug (195101) — which, helpfully, is callable under
 * Atmosphere. The PlayTimerSettings layout is documented on PtSettings in
 * pctl_ops.h. v3.0.0 wrote the same bytes through a u16[34] view; for the same
 * input, compose_settings() below produces byte-identical output except that
 * unrestricted days keep the 06:00 bedtime-reset default instead of all-zero.
 * ------------------------------------------------------------------------- */

#define PT_UNSUPPORTED MAKERESULT(Module_Libnx, LibnxError_IncompatSysVer)

static void rep(char **p, char *end, const char *fmt, ...)
{
    if (*p >= end) return;
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(*p, (size_t)(end - *p), fmt, ap);
    va_end(ap);
    if (n > 0) { *p += n; if (*p > end) *p = end; }
}

static Result read_settings(PtSettings *out)
{
    memset(out, 0, sizeof(*out));
    if (!pctl_fw_play_timer_supported()) return PT_UNSUPPORTED;
    pctl_ops_reinit();
    return serviceDispatchOut(pctlGetServiceSession_Service(), 145601, *out);   // GetPlayTimerSettings
}

static Result write_settings(const PtSettings *s)
{
    if (!pctl_fw_play_timer_supported()) return PT_UNSUPPORTED;
    pctl_ops_reinit();
    Result rc = serviceDispatchIn(pctlGetServiceSession_Service(), 195101, *s);   // SetPlayTimerSettingsForDebug
    const u8 *b = (const u8 *)s;
    char hex[0x44 * 2 + 1];
    for (int i = 0; i < 0x44; i++) snprintf(hex + i * 2, 3, "%02X", b[i]);
    pctl_logf("195101 SetPlayTimerSettingsForDebug rc=0x%08X data=%s", (unsigned)rc, hex);
    return rc;
}

// The slot a given weekday is read from. This tool always writes the per-day
// slots, but a config synced from the companion app in "same every day" mode
// may only fill the daily slot — fall back to it when every per-day slot is empty.
static const PtDaySetting *day_slot(const PtSettings *s, int n)
{
    static const PtDaySetting zero;
    for (int k = 0; k < 7; k++)
        if (memcmp(&s->weekly[k], &zero, sizeof(zero)) != 0) return &s->weekly[n];
    return &s->daily;
}

static void decode_bedtime(const PtDaySetting *d, PtBedtime *out)
{
    out->enabled      = d->bedtime_enabled != 0;
    out->hour         = d->bedtime_hour;
    out->minute       = d->bedtime_minute;
    out->reset_hour   = d->bedtime_reset_hour;
    out->reset_minute = d->bedtime_reset_minute;
}

// Builds a complete PlayTimerSettings from per-day limits + one bedtime alarm
// applied to every day. Header values are the ones seen in the companion-app dump.
static void compose_settings(PtSettings *s, const u16 days_min[7], const PtBedtime *bed)
{
    memset(s, 0, sizeof(*s));
    bool any = bed->enabled;
    for (int n = 0; n < 7; n++) if (days_min[n] != PT_DAY_NOLIMIT) any = true;
    if (!any) return;   // all-zero struct -> the play timer is turned off, IsPlayTimerEnabled becomes false

    s->enabled = 1;
    s->hdr1    = 1;
    s->hdr2    = 1;
    // s->daily stays all-zero, as in the dump (the per-day slots are used)
    for (int n = 0; n < 7; n++) {
        PtDaySetting *d = &s->weekly[n];
        d->bedtime_enabled      = bed->enabled ? 1 : 0;
        d->bedtime_hour         = bed->enabled ? bed->hour   : 0;
        d->bedtime_minute       = bed->enabled ? bed->minute : 0;
        d->bedtime_reset_hour   = bed->enabled ? bed->reset_hour   : 6;
        d->bedtime_reset_minute = bed->enabled ? bed->reset_minute : 0;
        if (days_min[n] != PT_DAY_NOLIMIT) {
            d->limit_enabled = 1;
            d->limit_minutes = days_min[n];   // 0 == fully blocked that day
        }
    }
}

void pctl_play_timer_query(PtState *out)
{
    memset(out, 0, sizeof(*out));
    for (int n = 0; n < 7; n++) out->day_min[n] = PT_DAY_NOLIMIT;
    out->supported = pctl_fw_play_timer_supported();
    if (!out->supported) return;

    pctl_ops_reinit();
    Service *srv = pctlGetServiceSession_Service();

    bool b = false;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1453, b))) out->enabled    = b;   // IsPlayTimerEnabled
    b = false;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1455, b))) out->restricted = b;   // IsRestrictedByPlayTimer
    u64 rem = 0;
    if (R_SUCCEEDED(serviceDispatchOut(srv, 1454, rem))) out->remaining_ns = rem;   // GetPlayTimerRemainingTime (ns)

    if (R_SUCCEEDED(read_settings(&out->raw))) {
        out->valid = true;
        for (int n = 0; n < 7; n++) {
            const PtDaySetting *d = day_slot(&out->raw, n);
            out->day_min[n] = d->limit_enabled ? d->limit_minutes : PT_DAY_NOLIMIT;
        }
        decode_bedtime(day_slot(&out->raw, 0), &out->bedtime);
    }
}

// Current per-day limits + bedtime (so one of them can be changed while keeping
// the other). Fails if the settings can't be read — never write blind.
static Result current_config(u16 days_min[7], PtBedtime *bed)
{
    PtSettings cur;
    Result rc = read_settings(&cur);
    if (R_FAILED(rc)) return rc;
    for (int n = 0; n < 7; n++) {
        const PtDaySetting *d = day_slot(&cur, n);
        days_min[n] = d->limit_enabled ? d->limit_minutes : PT_DAY_NOLIMIT;
    }
    decode_bedtime(day_slot(&cur, 0), bed);
    if (!cur.enabled) bed->enabled = false;
    return 0;
}

Result pctl_play_timer_set_days(const u16 days_min[7])
{
    u16 old_days[7];
    PtBedtime bed;
    Result rc = current_config(old_days, &bed);
    if (R_FAILED(rc)) return rc;

    PtSettings s;
    compose_settings(&s, days_min, &bed);
    return write_settings(&s);
}

Result pctl_play_timer_set_uniform(u16 minutes)
{
    u16 d[7];
    for (int i = 0; i < 7; i++) d[i] = minutes;
    return pctl_play_timer_set_days(d);
}

Result pctl_play_timer_clear(void)
{
    u16 d[7];
    for (int i = 0; i < 7; i++) d[i] = PT_DAY_NOLIMIT;   // every day unrestricted
    return pctl_play_timer_set_days(d);
}

Result pctl_play_timer_set_bedtime(const PtBedtime *bed)
{
    u16 days[7];
    PtBedtime old;
    Result rc = current_config(days, &old);
    if (R_FAILED(rc)) return rc;

    PtSettings s;
    compose_settings(&s, days, bed);
    return write_settings(&s);
}

Result pctl_play_timer_write_raw(const PtSettings *s)
{
    return write_settings(s);
}

void pctl_bedtime_readback(PtBedtime *out, bool *ok)
{
    memset(out, 0, sizeof(*out));
    *ok = false;
    if (!pctl_fw_play_timer_supported()) return;
    pctl_ops_reinit();
    Service *srv = pctlGetServiceSession_Service();

    bool en = false;
    u8 h = 0, m = 0, rh = 0, rm = 0;
    if (R_FAILED(serviceDispatchOut(srv, 1954, en))) return;   // IsBedtimeAlarmEnabled
    if (R_FAILED(serviceDispatchOut(srv, 1956, h)))  return;   // GetBedtimeAlarmTimeHour
    if (R_FAILED(serviceDispatchOut(srv, 1957, m)))  return;   // GetBedtimeAlarmTimeMinute
    if (R_FAILED(serviceDispatchOut(srv, 1958, rh))) return;   // GetBedtimeAlarmResetTimeHour
    if (R_FAILED(serviceDispatchOut(srv, 1959, rm))) return;   // GetBedtimeAlarmResetTimeMinute
    out->enabled = en;
    out->hour = h; out->minute = m; out->reset_hour = rh; out->reset_minute = rm;
    *ok = true;
}

static void rep_clock(char **p, char *e, const char *name, TimeType type)
{
    u64 ts = 0;
    Result r = timeGetCurrentTime(type, &ts);
    rep(p, e, "%-22s: rc=0x%08X", name, (unsigned)r);
    if (R_SUCCEEDED(r)) {
        time_t t = (time_t)ts;
        struct tm *tm = gmtime(&t);
        if (tm) rep(p, e, "  %04d-%02d-%02d %02d:%02d:%02d UTC", tm->tm_year + 1900, tm->tm_mon + 1,
                    tm->tm_mday, tm->tm_hour, tm->tm_min, tm->tm_sec);
    }
    rep(p, e, "\n");
}

void pctl_play_timer_dump(char *buf, size_t bufsz)
{
    if (bufsz == 0) return;
    char *p = buf, *e = buf + bufsz;
    buf[0] = '\0';

    u32 hv = hosversionGet();
    rep(&p, e, "=== Pctl Manager diagnostic dump (read-only) ===\n");
    rep(&p, e, "firmware %u.%u.%u   play-timer commands supported: %s\n",
        HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv),
        pctl_fw_play_timer_supported() ? "yes" : "NO (needs 21.0.0+)");
    rep_clock(&p, e, "UserSystemClock",    TimeType_UserSystemClock);
    rep_clock(&p, e, "NetworkSystemClock", TimeType_NetworkSystemClock);
    rep_clock(&p, e, "LocalSystemClock",   TimeType_LocalSystemClock);

    Result ir = pctl_ops_reinit();
    if (R_FAILED(ir)) { rep(&p, e, "pctlInitialize failed: 0x%08X\n", (unsigned)ir); return; }
    Service *srv = pctlGetServiceSession_Service();

    // Restriction state + PIN (digits masked: '##' for any ASCII '0'..'9' byte).
    { bool b = false; Result r = serviceDispatchOut(srv, 1031, b);
      rep(&p, e, "\n1031 IsRestrictionEnabled          : rc=0x%08X  %s\n", (unsigned)r, R_SUCCEEDED(r) ? (b ? "true" : "false") : "-"); }
    { bool b = false; Result r = serviceDispatchOut(srv, 1006, b);
      rep(&p, e, "1006 IsRestrictionTemporaryUnlocked: rc=0x%08X  %s\n", (unsigned)r, R_SUCCEEDED(r) ? (b ? "true" : "false") : "-"); }
    { u32 len = 0; Result r = serviceDispatchOut(srv, 1206, len);
      rep(&p, e, "1206 GetPinCodeLength              : rc=0x%08X  len=%u\n", (unsigned)r, (unsigned)len); }
    pctl_ops_reinit(); srv = pctlGetServiceSession_Service();
    { u8 b[0x10]; memset(b, 0xCC, sizeof(b)); u32 ret = 0;
      Result r = serviceDispatchOut(srv, 1208, ret,
          .buffer_attrs = { SfBufferAttr_HipcPointer | SfBufferAttr_Out },
          .buffers      = { { b, sizeof(b) } });
      rep(&p, e, "1208 GetPinCode                    : rc=0x%08X  ret=%u   (CC == untouched, ## == ASCII digit)\n ", (unsigned)r, (unsigned)ret);
      for (size_t i = 0; i < sizeof(b); i++) rep(&p, e, (b[i] >= '0' && b[i] <= '9') ? "## " : "%02X ", b[i]);
      rep(&p, e, "\n"); }

    if (!pctl_fw_play_timer_supported()) {
        rep(&p, e, "\n(play-timer section skipped: firmware too old)\n");
        return;
    }

    pctl_ops_reinit(); srv = pctlGetServiceSession_Service();
    { bool b = false; Result r = serviceDispatchOut(srv, 1453, b);
      rep(&p, e, "\n1453 IsPlayTimerEnabled       : rc=0x%08X  %s\n", (unsigned)r, R_SUCCEEDED(r) ? (b ? "true" : "false") : "-"); }
    { bool b = false; Result r = serviceDispatchOut(srv, 1455, b);
      rep(&p, e, "1455 IsRestrictedByPlayTimer  : rc=0x%08X  %s\n", (unsigned)r, R_SUCCEEDED(r) ? (b ? "true" : "false") : "-"); }
    { bool b = false; Result r = serviceDispatchOut(srv, 1458, b);
      rep(&p, e, "1458 IsPlayTimerAlarmDisabled : rc=0x%08X  %s\n", (unsigned)r, R_SUCCEEDED(r) ? (b ? "true" : "false") : "-"); }
    { u64 v = 0; Result r = serviceDispatchOut(srv, 1454, v);
      rep(&p, e, "1454 GetPlayTimerRemainingTime: rc=0x%08X  0x%016llX", (unsigned)r, (unsigned long long)v);
      if (R_SUCCEEDED(r) && v) rep(&p, e, "  (~%llu min if ns)", (unsigned long long)(v / 60000000000ULL));
      rep(&p, e, "\n"); }
    pctl_ops_reinit(); srv = pctlGetServiceSession_Service();
    { u64 v = 0; Result r = serviceDispatchOut(srv, 1952, v);
      rep(&p, e, "1952 GetPlayTimerSpentTimeForTest: rc=0x%08X  0x%016llX\n", (unsigned)r, (unsigned long long)v); }

    // GetPlayTimerSettings (145601): raw bytes + decode.
    pctl_ops_reinit(); srv = pctlGetServiceSession_Service();
    { u8 b[0x44]; memset(b, 0xCC, sizeof(b));
      Result r = serviceDispatchOut(srv, 145601, b);
      rep(&p, e, "\n145601 GetPlayTimerSettings: rc=0x%08X   (0x44 bytes; CC == server left it)\n", (unsigned)r);
      for (int i = 0; i < 0x44; i++) { rep(&p, e, "%02X ", b[i]); if ((i & 15) == 15) rep(&p, e, "\n"); }
      rep(&p, e, "\n");
      if (R_SUCCEEDED(r)) {
          PtSettings s; memcpy(&s, b, sizeof(s));
          static const char *const dn[8] = { "daily", "Sun", "Mon", "Tue", "Wed", "Thu", "Fri", "Sat" };
          rep(&p, e, "header: enabled=%u hdr1=%u hdr2=%u hdr3=%u\n", s.enabled, s.hdr1, s.hdr2, s.hdr3);
          rep(&p, e, "slot   bed  time   reset  limit  minutes\n");
          for (int k = 0; k < 8; k++) {
              const PtDaySetting *d = (k == 0) ? &s.daily : &s.weekly[k - 1];
              rep(&p, e, "%-5s  %3u  %02u:%02u  %02u:%02u  %5u  %7u\n", dn[k], d->bedtime_enabled,
                  d->bedtime_hour, d->bedtime_minute, d->bedtime_reset_hour, d->bedtime_reset_minute,
                  d->limit_enabled, d->limit_minutes);
          }
      } }

    // Bedtime getters (1954-1959) — the system's own interpretation, to check the layout above.
    pctl_ops_reinit(); srv = pctlGetServiceSession_Service();
    { bool b = false; Result r = serviceDispatchOut(srv, 1954, b);
      rep(&p, e, "\n1954 IsBedtimeAlarmEnabled         : rc=0x%08X  %s\n", (unsigned)r, R_SUCCEEDED(r) ? (b ? "true" : "false") : "-"); }
    static const u32 bed_cmds[4] = { 1956, 1957, 1958, 1959 };
    static const char *const bed_names[4] = { "GetBedtimeAlarmTimeHour", "GetBedtimeAlarmTimeMinute",
                                              "GetBedtimeAlarmResetTimeHour", "GetBedtimeAlarmResetTimeMinute" };
    for (int i = 0; i < 4; i++) {
        pctl_ops_reinit(); srv = pctlGetServiceSession_Service();
        u8 v = 0xCC; Result r = serviceDispatchOut(srv, bed_cmds[i], v);
        rep(&p, e, "%u %-30s: rc=0x%08X  %u\n", (unsigned)bed_cmds[i], bed_names[i], (unsigned)r, v);
    }

    // GetPlayTimerRemainingTimeDisplayInfo (1459, fw 20+) — 0x20 bytes on fw 22.1.0.
    pctl_ops_reinit(); srv = pctlGetServiceSession_Service();
    { u8 b[0x20]; memset(b, 0xCC, sizeof(b));
      Result r = serviceDispatchOut(srv, 1459, b);
      rep(&p, e, "\n1459 GetPlayTimerRemainingTimeDisplayInfo: rc=0x%08X  (0x20 bytes; CC == server left it)\n", (unsigned)r);
      if (R_SUCCEEDED(r))
          for (int i = 0; i < 0x20; i++) { rep(&p, e, "%02X ", b[i]); if ((i & 15) == 15) rep(&p, e, "\n"); }
      rep(&p, e, "\n"); }
}
