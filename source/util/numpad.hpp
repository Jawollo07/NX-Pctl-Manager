// numpad — thin wrapper around the system swkbd applet for the play-timer's
// "enter minutes per day" prompts (0..1440 range, NumPad layout).
// Copyright (C) 2026 Taylor.  GPLv3-or-later (see LICENSE).
#pragma once

#include <cstdint>
#include <optional>
#include <string>

namespace numpad
{

// Pops the system NumPad applet. Returns the entered minute value (clamped
// 0..1440) when the user confirms; std::nullopt when they cancel or swkbd
// fails to open. Blocks while the applet is up (it overlays our window).
std::optional<uint16_t> prompt_minutes(const std::string& header, uint16_t current);

// Time of day as HHMM digits (e.g. "2100"). Returns {hour, minute} when the
// input is a valid 00:00..23:59 time; std::nullopt on cancel or invalid input.
struct HourMinute { uint8_t hour, minute; };
std::optional<HourMinute> prompt_hhmm(const std::string& header, HourMinute current);

// Masked NumPad entry for the launch-time PIN check. std::nullopt on cancel.
std::optional<std::string> prompt_pin(const std::string& header);

}   // namespace numpad
