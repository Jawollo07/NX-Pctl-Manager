// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#include "util/numpad.hpp"

#include <cstdio>
#include <cstdlib>

#include <switch.h>   // SwkbdConfig / swkbdShow — libnx headers carry their own extern "C"

namespace numpad
{

std::optional<uint16_t> prompt_minutes(const std::string& header, uint16_t current)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return std::nullopt;

    swkbdConfigMakePresetDefault(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    swkbdConfigSetStringLenMin(&kbd, 1);
    swkbdConfigSetStringLenMax(&kbd, 4);
    swkbdConfigSetHeaderText(&kbd, header.c_str());
    swkbdConfigSetGuideText(&kbd, "minutes per day  (0 = no play that day; max 1440)");

    char initial[8];
    std::snprintf(initial, sizeof(initial), "%u", (unsigned)current);
    swkbdConfigSetInitialText(&kbd, initial);

    char out[16] = {0};
    Result rc    = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return std::nullopt;   // cancelled

    long v = std::strtol(out, nullptr, 10);
    if (v < 0)    v = 0;
    if (v > 1440) v = 1440;
    return (uint16_t)v;
}

std::optional<HourMinute> prompt_hhmm(const std::string& header, const std::string& invalid_header,
                                      HourMinute current)
{
    char initial[16];
    std::snprintf(initial, sizeof(initial), "%02u%02u", (unsigned)current.hour, (unsigned)current.minute);
    const std::string* shown = &header;

    for (;;) {
        SwkbdConfig kbd;
        if (R_FAILED(swkbdCreate(&kbd, 0))) return std::nullopt;

        swkbdConfigMakePresetDefault(&kbd);
        swkbdConfigSetType(&kbd, SwkbdType_NumPad);
        swkbdConfigSetStringLenMin(&kbd, 3);
        swkbdConfigSetStringLenMax(&kbd, 4);
        swkbdConfigSetHeaderText(&kbd, shown->c_str());
        swkbdConfigSetGuideText(&kbd, "HHMM, 24-hour  (e.g. 2100 = 9:00 pm)");
        swkbdConfigSetInitialText(&kbd, initial);

        char out[16] = {0};
        Result rc    = swkbdShow(&kbd, out, sizeof(out));
        swkbdClose(&kbd);
        if (R_FAILED(rc)) return std::nullopt;   // cancelled

        long v = std::strtol(out, nullptr, 10);
        long h = v / 100, m = v % 100;
        if (v >= 0 && h <= 23 && m <= 59) return HourMinute{ (uint8_t)h, (uint8_t)m };

        // Not a time (e.g. 2460): ask again, keeping what was typed.
        std::snprintf(initial, sizeof(initial), "%s", out);
        shown = &invalid_header;
    }
}

std::optional<std::string> prompt_pin(const std::string& header)
{
    SwkbdConfig kbd;
    if (R_FAILED(swkbdCreate(&kbd, 0))) return std::nullopt;

    swkbdConfigMakePresetPassword(&kbd);
    swkbdConfigSetType(&kbd, SwkbdType_NumPad);
    swkbdConfigSetStringLenMin(&kbd, 4);
    swkbdConfigSetStringLenMax(&kbd, 8);
    swkbdConfigSetHeaderText(&kbd, header.c_str());

    char out[16] = {0};
    Result rc    = swkbdShow(&kbd, out, sizeof(out));
    swkbdClose(&kbd);
    if (R_FAILED(rc)) return std::nullopt;
    return std::string(out);
}

}   // namespace numpad
