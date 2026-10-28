# MVP: Require local TOTP before using the Nintendo Switch

Status: **implementation plan only**. This document narrows the immediate project scope to one goal: require a valid local TOTP code before the console can be used. It does not claim that a system-wide lock has already been implemented.

## Goal and acceptance criteria

- On boot and wake, show a dedicated lock screen before normal console use.
- A valid offline TOTP code grants a clearly defined unlock session.
- HOME, B, closing a homebrew app, or restarting the UI must not bypass the lock.
- The lock/enforcement mechanism must live outside the ordinary NRO process.
- A one-time parent recovery code can restore access if the authenticator is lost.
- No network, Nintendo account, or phone connection is required to verify TOTP.
- Unknown firmware/Atmosphère versions must not enable untested system hooks.

## Important current limitation

The current C++/libnx/Borealis NRO asks for TOTP when opening the homebrew app. That protects the app's own menu only. It cannot prevent someone from closing the app and using the HOME menu, launching other software, or changing system settings.

The current project already has TOTP enrollment/verification and a pctl adapter for parental-control settings. Neither of those provides a persistent system-wide lock. A Tesla overlay or ordinary homebrew app is also not enough because it can be dismissed or exited.

## Minimal target architecture

1. **Lock-screen UI** — C++/Borealis presentation and numeric TOTP entry. Reuse the current TOTP verifier where appropriate, but separate the lock UI from the parent settings UI.
2. **Local auth module** — RFC 6238 TOTP, fixed documented parameters shared with common authenticator apps, a small clock-skew window, throttling, no secret logging, and a one-time recovery code.
3. **Resident Atmosphère component (new)** — remains active independently of the NRO, exposes a narrow versioned IPC interface, and owns lock state. Closing the UI must not stop enforcement.
4. **HOME/qlaunch integration (new, feasibility-critical)** — displays the lock before normal use and prevents normal navigation from dismissing it. This needs a firmware-specific proof of concept; the exact maintainable integration point is not yet established.
5. **Safe recovery** — a one-time high-entropy recovery code can rotate the TOTP secret. Store only a salted verifier using a standard KDF available in the project. Rate-limit attempts and invalidate the old recovery code after use. Provide a tested service-disable/uninstall recovery route.

## Unlock semantics

For the first MVP, choose a simple explicit policy: successful TOTP unlocks the console for a configurable session (initially proposed as “until the next sleep or reboot”). The resident component must relock on the selected lifecycle events. This behaviour must be verified on the supported firmware rather than assumed.

The lock must not patch Nintendo's native parental-control authentication to accept TOTP. The custom lock is an additional app-owned gate; native Nintendo PIN behaviour remains unchanged.

## Security boundary

Offline TOTP is practical, but the SD card is not a hardware-backed secret store. A person who can read or modify the SD card, run privileged homebrew, or change CFW can copy the secret or bypass a custom lock. This design is a parental-control mechanism for normal console use, not a tamper-proof boundary against someone with full physical/CFW control.

## Implementation milestones

### Milestone 1 — feasibility spike before broad code changes

On one explicitly supported Atmosphère/Horizon version, prove:
- a resident component can start and survive closing the NRO;
- the current title/system state can be observed reliably enough to know when the lock should apply;
- a lock surface can be shown through a maintainable HOME/qlaunch integration point;
- HOME/B and ordinary navigation cannot dismiss the lock;
- sleep/wake, reboot, crash, and safe recovery work without a boot loop.

Do not claim a system-wide lock until all of these pass on-device. If qlaunch integration cannot reliably enforce the lock, stop and document that limitation instead of shipping a false sense of security.

### Milestone 2 — minimal implementation

- Reuse/refactor the current local TOTP verifier.
- Add the resident lock-state service and versioned IPC.
- Add lock-screen UI and session/relock state machine.
- Add recovery-code setup and single-use rotation.
- Keep unrelated features (play-time budgets, history, per-title restrictions, warnings) out of this MVP.

### Milestone 3 — hardening

Test valid/invalid TOTP, clock drift and rollback, rate limiting, repeated failures, lost authenticator recovery, corrupt configuration, service crash, UI crash, sleep/wake, reboot, unsupported firmware, and safe uninstall. Publish a compatibility matrix and make unsupported firmware fail closed without risking a boot loop.

## Explicitly deferred

Daily and weekday limits, bedtime windows, play-time accounting, usage history, notifications, per-title policies, and general parental-control dashboard improvements are deferred until the system-wide TOTP lock is demonstrated and stable.
