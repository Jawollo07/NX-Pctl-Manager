# Architecture: Full Nintendo Switch Parental Controls

Status: **architecture proposal**. The current project is a C++/libnx/Borealis homebrew application. This document does not claim that a system-wide lock or a new Atmosphère module already exists.

## Goals

- Keep the current Borealis-based NRO as the on-console parent configuration UI.
- Use an offline, local TOTP authenticator for the parent area.
- Enforce daily limits, per-weekday limits, play windows/bedtime, per-title rules where supported, warnings, and a local usage history.
- Keep restrictions enforced when the configuration UI is closed; exiting the NRO must not stop enforcement.
- Provide a secure recovery code and a documented recovery flow.
- Prefer documented Horizon/libnx interfaces. Isolate reverse-engineered or firmware-specific behaviour behind adapters and feature gates.

## Current state versus planned work

### Present in the current homebrew codebase (verify against the checked-out commit before release)

- C++ application using libnx and Borealis.
- A pctl service adapter for existing parental-control operations.
- Daily play timer configuration, including per-weekday values and bedtime/play-timer options exposed by the current pctl implementation.
- Status/diagnostics and parental-control maintenance actions.
- Local TOTP enrollment/verification for access to the homebrew UI, using a secret file on the SD card.

These features configure or inspect Nintendo's parental-control subsystem and protect this application's own UI. They do **not** constitute an independent system-wide lock. The TOTP code does not replace Nintendo's native PIN prompt.

### Not implemented by the current NRO

- A resident background enforcement component.
- A persistent lock screen that survives closing the homebrew UI and applies to HOME-menu/system navigation.
- A policy engine for arbitrary per-title allow/deny rules, with reliable foreground-title detection.
- A first-class local history database, warning scheduler, or robust clock rollback handling.
- Recovery-code enrollment, rate limiting, rotation, and tested recovery procedures.

## Proposed component model

- **NRO / Borealis parent UI:** policy editor, status, TOTP enrollment/verification, recovery-code setup, diagnostics.
- **Versioned local IPC (to be designed and validated):** narrow interface between UI and resident service.
- **New Atmosphère integration:** resident policy/enforcement service, title/session and clock adapters, usage accounting, warning scheduler, lock-state API.
- **Existing pctl adapter:** supported Nintendo parental-control operations, isolated from custom policy enforcement.
- **Separate HOME/qlaunch integration:** research and proof of concept for a persistent system lock surface.

The target architecture is not a claim that a generic sysmodule can already display a persistent HOME-menu lock. The exact mechanism to render a lock over system UI and prevent navigation must be established with a firmware-specific proof of concept. A Tesla overlay or an ordinary homebrew app alone is not a system-wide lock. Keep enforcement separate from the UI so closing/restarting the UI cannot stop the policy engine.

## Module boundaries

### 1. ui/ — existing C++/Borealis app

- Show parent dashboard, current rule status, remaining time, history and diagnostics.
- Require successful local TOTP authentication before exposing parent-only actions.
- Edit a draft policy, validate it, then submit one atomic update.
- Never perform enforcement only inside the UI process.

### 2. auth/ — local authentication

- RFC 6238 TOTP; recommended parameters: HMAC-SHA-1, six digits, 30-second period, and a small explicit clock-skew window. Keep parameters documented and consistent with the authenticator app.
- Enrollment requires a one-time confirmation of the generated secret before enabling it.
- Apply retry throttling and a short delay after repeated failures; never log codes or secrets.
- Store the TOTP secret separately from non-secret policy data. The SD card is not a hardware-backed secret store: anyone able to read/modify it or run privileged homebrew may copy or bypass the secret.
- Recovery code: generate a high-entropy, one-time code at setup; display it once; store only a salted verifier using a standard password/KDF implementation available in the build, not a home-grown hash. Require the recovery code to rotate the TOTP secret and invalidate the old recovery code. Rate-limit attempts.
- Recovery protects against accidental loss, not a hostile person with full SD/CFW access. Document this threat boundary.

### 3. policy/ — platform-independent policy engine (new)

Represent rules as validated data, independent of Borealis and pctl:
- Per-day play budget (minutes) for all seven weekdays.
- Allowed play windows and bedtime, including windows that cross midnight.
- Per-title allow/deny rules keyed by stable application/title ID, only when title identity can be obtained reliably.
- Warning thresholds (for example 15, 5, and 1 minute remaining).
- Enforcement action: notify, block launching, or suspend current software only if the chosen platform interface is verified to support that action.

Define explicit precedence: hard deny/bedtime > allowed window > daily budget > per-title allow/deny policy; review final precedence before implementation. Missing/corrupt policy must fail safely without bricking the console: preserve a recovery route and never make recovery depend on the network.

### 4. daemon/ — resident Atmosphère component (new, high risk)

- Load and validate policy at boot; maintain an atomic last-known-good copy.
- Observe title launch/exit and elapsed foreground time through verified platform interfaces.
- Track usage against a monotonic elapsed-time source; use calendar time only to choose weekday/window.
- Enforce policy even when the NRO is closed.
- Expose a narrow, versioned local IPC interface: GetStatus, GetHistory, GetPolicy, SetPolicy, RequestParentUnlock, GetDiagnostics.
- Validate every IPC request; UI requests must not be able to arbitrarily disable the service.
- Keep the daemon alive independently from the UI and report health/version compatibility.

Do not begin broad system hooks before a minimal proof of concept demonstrates reliable title detection, clean boot, suspend/resume behaviour, and recovery after a crash. Prefer documented service APIs; isolate undocumented/reverse-engineered hooks by firmware/Atmosphère version and disable them on unknown versions.

### 5. system-lock/ — HOME/qlaunch integration (separate feasibility milestone)

This is the critical unknown. The goal is a lock surface that:
- appears at boot/wake or when policy requires it;
- remains above normal navigation and cannot be dismissed by HOME/B;
- grants temporary access only after local parent authentication;
- does not prevent access to recovery, system update, controller pairing, or other essential system functions.

Implement only after a proof of concept identifies a maintainable Atmosphère/qlaunch integration point for each supported firmware. Do not patch native Nintendo authentication to accept TOTP. The new lock should be an app-owned policy gate and leave Nintendo's native PIN behaviour unchanged. Unknown firmware versions should disable this feature rather than risk a boot loop.

### 6. storage/ — policy, history, diagnostics (new)

- Store versioned configuration under a dedicated /switch/nx-pctl-manager/ directory.
- Use write-to-temp + flush + atomic rename; retain one last-known-good policy.
- Keep a bounded append-only event history (launch, exit, warning, limit reached, lock/unlock, policy change, recovery) with timestamps and title IDs; never store authentication codes or secrets.
- Include schema version, migration logic, checksum for corruption detection, and a read-only diagnostic export.
- A checksum detects accidental corruption, not deliberate SD-card tampering.

## Time and accounting rules

- TOTP requires a reasonably correct local wall clock. Offline operation is supported, but time rollback can affect TOTP and weekday rules; detect large clock jumps and require parent review rather than silently resetting accumulated play time.
- Account elapsed usage using a monotonic clock while a title is foregrounded. Do not count time spent in system settings or asleep unless explicitly chosen.
- Suspend/resume and reboot accounting must be tested. If reliable elapsed-time recovery across reboot is not possible, persist session start/stop events and mark uncertain intervals in history rather than inventing precise usage.
- Define timezone source and daylight-saving behaviour for weekday windows; test midnight-crossing windows and daylight-saving transitions.

## Per-title limits: capability gate

First determine what title/session metadata is available to the resident component on each supported Atmosphère/Horizon version. If reliable current-title identity or launch interception is unavailable, expose the feature as unsupported rather than promising a block that can be bypassed. Keep the existing pctl adapter separate from custom per-title policy.

## Security and failure handling

- No network dependency for authentication, enforcement, or recovery.
- Least privilege: the UI can edit validated policy but cannot execute arbitrary commands through IPC.
- Rate-limit TOTP and recovery attempts; audit security events without recording secrets.
- Keep Nintendo's native PIN/authentication intact.
- Provide a documented safe mode/uninstall procedure and test recovery from corrupt config, daemon crash, failed update, and incompatible firmware.
- This is a CFW/homebrew control, not a tamper-proof boundary against an administrator who controls the SD card, CFW, or boot chain.

## Delivery plan

1. **Baseline audit:** inventory current pctl, TOTP, build, and firmware assumptions; add tests around existing behaviour. Do not change enforcement yet.
2. **Policy and storage library:** versioned schema, validation, atomic writes, history format, clock abstraction, unit tests on desktop.
3. **Parent UX:** TOTP enrollment/rotation, recovery-code lifecycle, policy editor, clear unsupported-feature states.
4. **Existing pctl integration:** map each policy setting to a documented/tested API; retain per-firmware capability checks and diagnostics.
5. **Resident-service feasibility spike:** prove title detection, IPC, boot lifecycle, suspend/resume, and crash recovery on one explicitly supported Atmosphère/Horizon version.
6. **Enforcement MVP:** daily budget + bedtime/play windows + warnings, with a tested recovery route.
7. **System lock feasibility spike:** separately prove HOME/qlaunch lock display and non-dismissability; only then implement the feature.
8. **Per-title policy:** add only after launch interception and title identity are verified.
9. **Release hardening:** CI build, desktop unit tests, on-console regression matrix, config migration tests, safe uninstall, and firmware compatibility table.

## Acceptance criteria

- Closing the parent NRO does not stop time accounting or enforcement.
- A valid TOTP is required for parent UI actions; invalid attempts are throttled.
- Recovery code is single-use and rotates both credentials without network access.
- Per-day budgets and overnight windows pass deterministic tests.
- Usage history is bounded, survives reboot, and marks uncertain intervals honestly.
- Corrupt policy restores the last-known-good policy and leaves a recovery route.
- System lock is labelled experimental until proven on each supported firmware.
- Unsupported title restrictions are shown as unsupported, never silently reported as enforced.
