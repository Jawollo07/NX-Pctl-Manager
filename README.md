# NX-Pctl-Manager

[![build](https://github.com/tailiang2008/NX-Pctl-Manager/actions/workflows/build.yml/badge.svg)](https://github.com/tailiang2008/NX-Pctl-Manager/actions/workflows/build.yml)
[![license: GPLv3](https://img.shields.io/badge/license-GPLv3-blue.svg)](LICENSE)
[![latest release](https://img.shields.io/github/v/release/tailiang2008/NX-Pctl-Manager)](https://github.com/tailiang2008/NX-Pctl-Manager/releases/latest)

**English** | [简体中文](README.zh-CN.md)

A Nintendo Switch parental-controls tool — **no phone app, no Nintendo account, no internet** — configure the daily play-time limit directly on the console, and reset / delete the PIN or unlink the companion app while you're at it.

> ⚠️ **Requires custom firmware (CFW; Atmosphère recommended).** It calls the restricted system `pctl` service directly, so it only runs on a hacked console (a stock retail console can't use it). It doesn't bypass any account or online verification — it just brings the parental-controls settings that are otherwise buried in the phone app / deep in System Settings onto the console, for CFW users to do offline.
>
> It talks to restricted / `*ForDebug` `pctl` commands directly. **Needs firmware 21.0.0 or newer. Tested on 22.5.0 / Atmosphère 1.12.0 (v3.1.0) and 22.1.0 / Atmosphère 1.11.1 (v3.0.0); other versions are unverified. Use at your own risk.** How `pctl` treats these writes is documented in [`docs/pctl-reverse-engineering.md`](docs/pctl-reverse-engineering.md).

---

## Features

### Configure the play timer (daily limit) offline — the main point of this tool

Nintendo **only lets you set the daily play-time limit through the "Nintendo Switch Parental Controls" phone app** and sync it to the console; there's no option for it in the console's System Settings. This tool talks to the console's `pctl` service directly, so **on the console, fully offline** (no phone app, no Nintendo account, no internet) you can:

- **Set one limit for every day** — one minute value applied to Sunday–Saturday.
- **Set each day separately** — Sunday, Monday … Saturday each get their own value (edited day-by-day in a sub-menu, staged, then saved all at once).
- **Turn the limit off** — remove the daily limit (a bedtime alarm, if set, is kept).
- **Bedtime alarm** — restrict play from a set time until the next morning (default end: 06:00), on every day.
- **When time is up** — suspend the software (default) or only show a notification.

Notes: `0` minutes means that day is **fully blocked** (not "no limit"); a single day can be set back to "no limit" in the per-day editor. A **parental-control PIN is required**: without one, `pctl` silently discards play-timer settings.

> **Offline consoles: set the network clock.** `pctl` only runs the timer when the console's network clock is valid, and a console that never reaches Nintendo's time servers (offline, or with Nintendo blocked) never gets one. The app detects this at launch and offers to set it from the system time. The same option is in the main menu. The timer then counts on the network clock, so changing the date in System Settings doesn't reset it.
>
> If the timer is already counting down, every write first pauses parental controls (cmd 1201), writes, and switches them straight back on (cmd 1007). You don't need to do anything.

### Other (parental-controls recovery / maintenance)

- **Set / change the PIN** — opens the system's parental-controls passcode screen.
- **Delete all parental controls** — wipes the PIN and every restriction (**irreversible**; confirmed before it runs).
- **Unlink the companion app** — breaks the link between the "Nintendo Switch Parental Controls" phone app and this console.
- **View status** — firmware, PIN, restrictions, play-timer status and network-clock status.
- **Diagnostic report** — a read-only dump saved to `sd:/switch/nx_pctl_dump.txt` (PIN digits masked). Attach it to bug reports, together with `sd:/switch/nx_pctl_manager.log` and `sd:/switch/nx_pctl_borealis.log`.

### PIN check at launch

When a PIN is registered, the app asks for it before showing the menu. Five wrong attempts, or B, exits. This stops a child from using the tool to lift the limit. As a result, the tool can no longer recover a forgotten PIN. Use Nintendo's own master-key reset instead (System Settings → Parental Controls → Forgot PIN).

### Central-server launch authentication

On builds compiled with `-DNX_PCTL_REMOTE_CONTROL=ON`, every launch authenticates the registered device against the central control server over HTTPS. If a console parental-control PIN is configured, the app also checks that PIN locally; the PIN is never transmitted to the server. The server uses the device token from `sd:/switch/nx_pctl_server.conf`. The old authenticator-based gate has been removed.

The remote-enabled build fails closed when the server cannot be reached or the device token is rejected. There is currently no offline grace period. Builds without the remote-control option retain the local console-PIN gate and do not perform server authentication.

## Central Control Server (experimental)

A separate implementation branch, `feature/central-control-server`, adds an optional FastAPI control server and an opt-in HTTPS Switch client. The default build and `main` branch remain unchanged. The server supports one-time device enrollment, per-device bearer credentials, policy versioning, status reports, an admin API, and a small browser dashboard.

Start with [docs/central-control-server.md](docs/central-control-server.md). The Switch HTTPS client requires a Switch libcurl build and is enabled with `-DNX_PCTL_REMOTE_CONTROL=ON`. Create `sd:/switch/nx_pctl_server.conf` with the HTTPS URL and the credentials returned at enrollment, and place a trusted PEM CA bundle at `sd:/switch/nx_pctl_ca.pem`. The default `apply_policy=0` fetches policy without changing the console; only `daily_limit_minutes` is currently supported for opt-in application. Bedtime policies are not yet applied by the Switch client. Network failures leave existing local settings unchanged.

**Security:** serve the API behind a public HTTPS reverse proxy; use separate long random admin and enrollment credentials; never send the console PIN to the server. A device token stored on the SD card can be copied by anyone with SD access.

## How to use

Controls: ↑ / ↓ move the cursor, (A) confirms, (B) goes back one level / (on the main menu) exits. Destructive actions (delete all parental controls, unlink companion app, remove the limit) show a confirmation screen first.

### Pick the flow that matches your console

**A. Parental controls haven't been set up on this console yet.** No companion-app pairing exists.

1. On the Switch: *System Settings* → *Parental Controls* → set a PIN. (Don't bother with the companion-app pairing — that's the whole point of this homebrew: you skip it.) The app's *Set / change parental control PIN* does the same.
2. Launch this app from hbmenu. If it reports that the network clock has never been set, choose *Set now*.
3. Continue at *Set the play timer* below.

**B. Parental controls were already set up via the Nintendo Switch Parental Controls phone app.** The phone app is currently paired with this console.

1. Launch this app from hbmenu, pick *Unlink companion app* — otherwise the phone app's next sync will overwrite whatever limit you set here.
2. Continue at *Set the play timer* below.

### Set the play timer

Main menu → *Play timer (daily limit / bedtime)*:

- *Set daily limit (all days)*: pops a number pad for the minutes (0–1440) and applies it to every day.
- *Per-day limits*: opens a sub-menu. Press (A) on a day to set its minutes or "No limit". Changes are **staged** and marked `(*)`; pick *Save per-day limits* to write all 7 at once, or press (B) to leave without saving.
- *Bedtime alarm*: set the start time and the next-morning end time (HHMM), or turn it off.
- *When time is up*: *Suspend software* or *Notify only*.
- *Remove play-time limit*: removes the daily limit (with confirmation).
- ⚠️ **Don't set the limit below "time already played today"** — the console locks as soon as it is written.

### Other actions

- *Set / change parental control PIN* switches to the system applet to set a passcode and returns automatically when done.
- *Delete all parental controls* wipes the PIN and every restriction (with confirmation, **irreversible**).
- *Set network clock from system time* seeds the network clock (see the offline note above).
- *Save diagnostic report* writes the read-only dump to the SD card.
- The main menu always shows a status panel; *Refresh status* re-reads it.

## Install

Grab `nx_pctl_manager.nro` from the [**Releases**](../../releases) page, drop it in `/switch/` on the SD card, and run it from hbmenu. (Or download `nx_pctl_manager.zip` from the same release and unzip it onto the SD card root — it puts the `.nro` in `/switch/` for you.)



The old SD-card key file `/switch/nx_pctl_totp.key`, if present from an earlier build, is no longer read by this version and can be deleted manually after upgrading.

## Build from source

Needs [devkitPro](https://devkitpro.org/) and the `switch-dev` toolchain (libnx included):

```sh
export DEVKITPRO=/opt/devkitpro
make            # produces ./nx_pctl_manager.nro
make dist       # packs nx_pctl_manager.zip — unzip it straight onto the SD card
make nxlink     # build and push over nxlink to a Switch running hbmenu on the LAN
make clean
```

Without a native toolchain, the repo's `./run.sh` builds via the official Docker image `devkitpro/devkita64`: `./run.sh` just builds; `./run.sh <switch-ip>` builds then pushes over nxlink (press Y in hbmenu first).

## License

GPLv3 (see [`LICENSE`](LICENSE)).

### Third-party

The graphical UI (v3.0.0+) is built on **[borealis](https://github.com/xfangfang/borealis)** — a Horizon-system-style UI library for Switch homebrew — under the Apache License 2.0. The pinned commit lives at `extern/borealis/`; see its `LICENSE` and `NOTICE`. The text-console v2 line did not depend on borealis.
