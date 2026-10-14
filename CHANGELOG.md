# Changelog

## v3.1.0

Fixes for every open issue, a bedtime alarm, and a PlayTimerSettings layout confirmed by reversing the fw 22.5.0 `pctl` sysmodule (see [`docs/pctl-reverse-engineering.md`](docs/pctl-reverse-engineering.md)).

### Fixes

- **The limit now applies right after writing it (#3, #6, #7).** v3.0.0 unlocked parental controls temporarily (cmd 1201) and never re-locked them. Every write now ends with cmd 1007 RevertRestrictionTemporaryUnlocked. The "Turn off & write" prompt is gone.
- **The timer counts down on offline consoles (#2).** pctl only enables the timer when the NetworkSystemClock is valid, and a console that can't reach Nintendo never sets it. The app now detects this at launch and offers to set the network clock from the system clock. The main menu has the same option.
- **Error 0x1088E when writing (#5).** This is pctl's "no PIN registered" result. A PIN is now required before any play-timer write; without one, pctl silently discards the settings anyway.
- Removing the limit goes through the same guarded write. In v3.0.0, removing it while the "time's up" screen was up could crash Atmosphère.
- Per-day editor: a day can be set back to "no limit".
- Play-timer reads and writes are refused below fw 21.0.0, where the struct has a different size.
- Fixed a start-up crash on fw 22.x. borealis rejects the `multiline` Label attribute, and the uncaught exception left its task thread running after exit (2168-0001).

### New

- **Bedtime alarm (#4).** Set a start time and the next-morning end time, applied to every day. The app reads the values back through pctl's own getters to confirm the write.
- **When time is up:** suspend the software (default) or notify only.
- **Launch-time PIN check.** The tool can lift every restriction, so it now asks for the console's parental-control PIN first.
- **Diagnostic report.** A read-only dump of firmware, clocks, the raw and decoded settings, and the bedtime getters is saved to `sd:/switch/nx_pctl_dump.txt`.
- **Logs on the SD card.** Writes go to `sd:/switch/nx_pctl_manager.log` and borealis output to `sd:/switch/nx_pctl_borealis.log`.
- Simplified Chinese translation.
- The `PROBE` build option was removed. Its diagnostic dump is now always available.

Tested on fw 22.5.0 / Atmosphère 1.12.0 (emuMMC).

## v3.0.0

UI rewrite onto [borealis](https://github.com/xfangfang/borealis) — Horizon-system-style graphical UI replaces the v2 text console. **No functional changes** — every action behaves the same.

- App display name is now **"Pctl Manager"** (repo name `NX-Pctl-Manager` unchanged).
- Non-CFW boot shows a dedicated error screen (was a silent fall-through in v2).
- Build switched to CMake; `make` / `./run.sh` interfaces unchanged.
- `.nro` grew 265 KB → ~8.2 MB (borealis library + assets).

Tested on fw 22.1.0 / Atmosphère 1.11.1. `make PROBE=1` still adds the diagnostic dump cell.

## v2.0.0

First public release. CFW (Atmosphère) only; tested on fw 22.1.0 / Atmosphère 1.11.1.

- **Configure the daily play-time limit offline** — one for all days, per-day limits, or off. When the timer is active, writes turn parental controls off temporarily (the app reads the PIN automatically) so the write doesn't destabilise Atmosphère.
- Set / change PIN via the system passcode applet.
- Delete all parental controls — also the recovery path when the PIN is forgotten.
- Unlink the Nintendo Switch Parental Controls companion phone app.
- View status: safety level, PIN length, restrictions, play-timer state.

`make PROBE=1` adds a read-only "Dump current config" diagnostic cell (off in release builds).
