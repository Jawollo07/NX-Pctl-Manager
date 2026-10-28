// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#include "view/status_panel.hpp"

#include <cstdio>
#include <string>

#include "app.hpp"
#include "util/pctl_ops_c.hpp"

namespace
{
// English labels — kept inline (i18n key clutter not worth it for short, fixed
// status strings). Switch to "@i18n/..." lookups when adding a second language.
constexpr const char* UNAVAILABLE = "(unavailable)";

std::string fmt_pin(const PctlStatus& s)
{
    if (!s.pin_length_ok) return UNAVAILABLE;
    if (s.pin_length == 0) return "not set";
    char buf[32];
    std::snprintf(buf, sizeof(buf), "set (%u digits)", (unsigned)s.pin_length);
    return buf;
}

// Reads as "Parental control enabled : <value>" — the value carries the
// configured-limit detail when the timer is on.
std::string fmt_firmware()
{
    u32 hv = hosversionGet();
    char buf[48];
    std::snprintf(buf, sizeof(buf), "%u.%u.%u%s", HOSVER_MAJOR(hv), HOSVER_MINOR(hv), HOSVER_MICRO(hv),
                  pctl_fw_play_timer_supported() ? "" : "  (play timer needs 21.0.0+)");
    return buf;
}

std::string fmt_clock()
{
    ClockStatus cs;
    clock_status(&cs);
    if (!cs.network_ok) return "NOT SET — timer won't count down";
    long long drift = (long long)cs.network - (long long)cs.user;
    if (cs.user_ok && (drift > 300 || drift < -300)) {
        char buf[48];
        std::snprintf(buf, sizeof(buf), "set (%+lld min vs system)", drift / 60);
        return buf;
    }
    return "set";
}

std::string fmt_play_timer(const PtState& pt)
{
    if (!pt.supported) return "n/a on this firmware";
    if (!pt.valid)    return UNAVAILABLE;
    if (!pt.enabled)  return "no";

    bool any = false, uniform = true;
    for (int i = 0; i < 7; i++) {
        if (pt.day_min[i] != PT_DAY_NOLIMIT)  any = true;
        if (pt.day_min[i] != pt.day_min[0])   uniform = false;
    }

    if (!any)                                    return "yes";
    if (uniform && pt.day_min[0] == 0)           return "yes — every day blocked";
    if (uniform) {
        char buf[32];
        std::snprintf(buf, sizeof(buf), "yes — %u min/day", (unsigned)pt.day_min[0]);
        return buf;
    }
    return "yes — per-day limits";
}
}   // namespace

StatusPanel::StatusPanel()
{
    pctl_logf("STATUS PANEL: constructor entered; inflating xml/view/status_panel.xml");
    this->inflateFromXMLRes("xml/view/status_panel.xml");
    pctl_logf("STATUS PANEL: XML inflation returned");
}

void StatusPanel::refresh()
{
    pctl_logf("STATUS PANEL: refresh entered");
    pctl_logf("STATUS PANEL: formatting firmware value");
    const std::string firmware = fmt_firmware();
    pctl_logf("STATUS PANEL: setting firmware label (length=%u)", (unsigned)firmware.size());
    this->firmware_value->setText(firmware);
    pctl_logf("STATUS PANEL: firmware label updated");

    pctl_logf("STATUS PANEL: formatting clock value");
    const std::string clock = fmt_clock();
    pctl_logf("STATUS PANEL: setting clock label (length=%u)", (unsigned)clock.size());
    this->clock_value->setText(clock);
    pctl_logf("STATUS PANEL: clock label updated");

    pctl_logf("STATUS PANEL: pctl_available=%d", app::pctl_available());
    if (!app::pctl_available()) {
        pctl_logf("STATUS PANEL: pctl service unavailable; setting fallback labels");
        this->pin_value->setText(UNAVAILABLE);
        this->restrictions_value->setText(UNAVAILABLE);
        this->play_timer_value->setText(UNAVAILABLE);
        return;
    }

    PctlStatus s{};
    pctl_logf("STATUS PANEL: calling pctl_status_fetch");
    pctl_status_fetch(&s);
    pctl_logf("STATUS PANEL: status fetched pin_ok=%d pin_length=%u restriction_ok=%d restriction_enabled=%d",
              s.pin_length_ok, s.pin_length, s.restriction_enabled_ok, s.restriction_enabled);
    const std::string pin_text = fmt_pin(s);
    pctl_logf("STATUS PANEL: setting PIN label");
    this->pin_value->setText(pin_text);
    pctl_logf("STATUS PANEL: PIN label updated");
    pctl_logf("STATUS PANEL: setting restrictions label");
    this->restrictions_value->setText(
        s.restriction_enabled_ok
            ? (s.restriction_enabled ? "yes" : "no")
            : UNAVAILABLE);
    pctl_logf("STATUS PANEL: restrictions label updated");

    PtState pt{};
    pctl_logf("STATUS PANEL: calling pctl_play_timer_query");
    pctl_play_timer_query(&pt);
    pctl_logf("STATUS PANEL: play timer result supported=%d valid=%d enabled=%d restricted=%d remaining_ns=%llu",
              pt.supported, pt.valid, pt.enabled, pt.restricted,
              (unsigned long long)pt.remaining_ns);
    const std::string timer_text = fmt_play_timer(pt);
    pctl_logf("STATUS PANEL: setting play timer label (length=%u)", (unsigned)timer_text.size());
    this->play_timer_value->setText(timer_text);
    pctl_logf("STATUS PANEL: refresh completed");
}

brls::View* StatusPanel::create() { return new StatusPanel(); }
