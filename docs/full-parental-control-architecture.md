# NX-Pctl-Manager — target architecture

This document describes the current project scope. It supersedes the earlier
local/offline TOTP lock proposal and any design that depends on Nintendo's
PCTL service.

## Product scope

The project is a backend-managed, headless Atmosphère sysmodule for global
console usage control. The backend is the authoritative control plane. The
Switch module is a small execution agent that synchronizes authenticated
policy snapshots, measures global application-active time and enforces the
current global decision.

The supported policy surface is console-wide only:

- global daily playtime limit;
- weekday-specific global limits;
- quiet hours;
- optional backend-issued parent-authentication grant;
- configured offline behavior.

There are no per-title/game rules, title histories, title IDs, local
administrator UI, or Nintendo PCTL calls. The running process ID is transient
and is used only for the PGL termination call.

## Components

1. **Backend** — FastAPI and SQLite own device enrollment, device credentials,
   policy revisions, event history, TOTP seeds, parent-authentication grants
   and revocation.
2. **Web dashboard** — same-origin browser UI at `/admin`; all management
   actions call the backend's authenticated admin API.
3. **Secure device protocol** — bounded JSON envelopes authenticated and
   encrypted with AES-GCM. HTTP metadata remains visible.
4. **Sysmodule** — headless Atmosphère module with local HIPC diagnostics,
   credential storage, trusted server clock, global usage accounting and PGL
   enforcement.
5. **Host tests and CI** — deterministic protocol/policy tests, backend
   regression tests and an experimental NSP build.

## Parent authentication

TOTP is configured and verified by the backend. The secret remains encrypted
on the backend and is never provisioned to the Switch. A successful code
creates a short-lived grant. The sysmodule retrieves only the grant state
through its authenticated encrypted device channel and enforces its expiry
using the trusted backend clock. The dashboard displays the TOTP seed only
once during setup; configure an authenticator app before closing that view.

## Security boundary

The system is designed to enforce policy during ordinary operation under the
supported Atmosphère configuration. It is not tamper-proof against a person
who controls the console, SD card, boot chain or custom firmware. A backend
revocation cannot reach a disconnected console until it reconnects. A host
build or CI result is not proof of runtime enforcement.

## Completion criteria

A production release requires all of the following:

- reproducible build from documented pinned toolchain revisions;
- cross-language known-answer tests for the complete secure envelope;
- enrollment and credential persistence tested across reboot;
- trusted clock, day rollover, clock regression and offline transitions tested;
- usage accounting verified through sleep/wake, HOME, user changes and
  application lifecycle transitions;
- PGL event coverage and termination timing verified on supported hardware;
- recovery from corrupt storage, failed update and interrupted boot tested;
- supported Atmosphère/Horizon matrix and installation/uninstall instructions;
- no secrets, runtime databases, caches or generated artifacts committed.

Until the hardware gates pass, releases must be labeled experimental and must
not claim an unbypassable whole-console lock.
