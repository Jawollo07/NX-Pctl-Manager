# Headless Atmosphere Sysmodule Migration Plan

Status: approved target architecture; implementation is not yet complete.

## 1. Accepted decisions

- Offline: keep the last verified policy in RAM and continue enforcing it while the network is unavailable.
- Communication: hybrid. HTTPS REST handles enrollment, authentication, full policy snapshots, heartbeats, event batches and reconciliation. WSS handles low-latency policy updates and events. REST polling is the fallback.
- Scope: full system-wide enforcement is the target, not merely a PCTL settings editor.
- Authority: the server owns durable policies, schedules, usage records and audit history. The Switch keeps only the active verified policy in RAM and the minimum persistent device credential/configuration needed to authenticate.

### Important RAM-only limitation

A RAM policy survives a network interruption only while the module remains alive. It does not survive a module crash, reboot or power loss. If startup occurs offline, the old policy cannot be reconstructed. The module must therefore remain in a restricted bootstrap state until it authenticates and fetches a policy. This is not optional if policy persistence is forbidden.

A unique device credential is not policy data. Unattended authentication requires a persistent per-device credential or certificate. Its storage must be threat-modelled and validated against the chosen Atmosphere release; do not assume hardware-backed storage exists.

## 2. Current repository baseline

The default branch is an NRO application:
- source/main.cpp starts Borealis, creates a window, pushes activities and runs the UI loop.
- source/activity, source/view, source/action and resources/xml are presentation code.
- extern/borealis is a build dependency.
- source/pctl_ops.c/.h is a useful PCTL adapter with firmware checks and documented command behaviour. Audit it and wrap it; do not blindly reuse every operation in a resident module.
- source/util/totp.* is homebrew-local TOTP code. Its SD-card secret handling must not be reused unchanged.
- CMakeLists.txt, Makefile and the GitHub Actions workflow package an NRO/ZIP, not an Atmosphere sysmodule.

Do not relabel the NRO as a sysmodule. A Stratosphere module needs its own entry point, module metadata, package layout, lifecycle, service registration, resource limits and installation method.

## 3. Target repository layout

Migrate additively. Keep the existing NRO build working on main until the sysmodule is independently buildable.

    sysmodule/
      source/
        main.cpp
        lifecycle/       startup, shutdown, suspend/resume
        ipc/             versioned local service and request validation
        network/         REST, WebSocket, TLS, reconnect
        identity/        device ID and credential provisioning
        policy/          schema, validation, evaluator, RAM snapshot
        monitor/         title/process and sleep/wake events
        enforcement/     PCTL and system integration adapters
        accounting/      monotonic play-time accounting
        telemetry/       bounded RAM event queue and redaction
      include/
      CMakeLists.txt
    common/
      protocol/
      model/
      crypto/
    docs/
      sysmodule-architecture.md
      sysmodule-migration-plan.md
      server-api.md
      policy-schema.md
      security-model.md
      firmware-compatibility.md
    atmosphere/          installable module metadata/package layout

The exact Stratosphere template, title ID, metadata and packaging must come from a pinned compatible Atmosphere source revision. Never guess title IDs, service names, kernel hooks or metadata fields.

## 4. Runtime design

1. Initialise required Horizon services, register the local IPC endpoint and start bounded workers.
2. Load the provisioned device identity. Missing/invalid credentials mean provisioning-required, never a shared default key.
3. Establish TLS with certificate and hostname validation; authenticate the device.
4. Fetch a complete policy snapshot over REST. Validate protocol version, device binding, revision, timestamps, limits, schedules and integrity metadata.
5. Atomically publish the verified policy as an immutable in-memory snapshot.
6. Connect WSS and subscribe using device ID and last accepted revision. Reject stale revisions and messages with an incorrect device scope.
7. Send heartbeat and event batches. On WSS loss, reconnect with bounded exponential backoff and jitter; REST remains the reconciliation path.
8. After reconnect, fetch/reconcile a full snapshot before applying deltas whose base revision is unknown.

Network loss rules:
- Keep enforcing the last verified in-memory policy.
- Never replace a valid policy with partial, malformed, expired or unauthenticated data.
- Queue events in bounded RAM; coalesce/drop low-priority telemetry under pressure and report loss counters after reconnect.
- Do not add a local policy DB, usage DB or persistent event log.
- On restart/power loss, RAM policy is gone; stay in the documented restricted bootstrap state until server authentication succeeds.

## 5. Proposed server protocol

These are proposed endpoints, not existing server routes. Freeze them only after the server implementation agrees.

REST:
- POST /v1/devices/enroll — one-time, short-lived enrollment code.
- POST /v1/devices/auth — device credential authentication/token exchange.
- GET /v1/devices/{id}/policy — complete, versioned policy snapshot.
- POST /v1/devices/{id}/heartbeat — health, uptime, firmware and current revision.
- POST /v1/devices/{id}/events:batch — bounded, deduplicated event batch.
- POST /v1/devices/{id}/policy-ack — accepted revision and application status.

WebSocket events:
- policy.updated — full snapshot or explicit base-revision delta.
- policy.revoked — force a fresh REST fetch; never apply a partial untrusted replacement.
- command.requested — strict allow-list of reversible actions; never arbitrary code or shell.
- server.ping / client.pong — keepalive.

Every message carries a protocol version. Each policy has a monotonically increasing revision, device binding, issue/expiry times and server time. Events have unique IDs and per-device sequence numbers so the server can deduplicate retries.

## 6. Policy and security rules

A policy needs at minimum:
- schema version, device ID, monotonically increasing revision;
- issued-at, not-after and server time;
- enabled state and explicit startup/offline behaviour;
- global and weekday play-time budgets;
- quiet hours and timezone semantics;
- allowed/blocked title identifiers and restriction mode;
- parent-authentication requirements.

Reject unknown critical schema versions, invalid bounds, stale revisions, cross-device replay and malformed input. Use monotonic time for elapsed playtime; use server time only for wall-clock schedules, with explicit clock uncertainty handling. TLS validation is mandatory. Never log credentials, tokens, PINs, TOTP secrets or authentication payloads.

## 7. Full system integration: feasibility gate

A resident sysmodule alone does not prove that users cannot close, bypass or kill the enforcement surface. Full scope requires supported mechanisms for:
- foreground title/process observation;
- enforcement before or immediately after a disallowed title starts;
- HOME, user changes, sleep/wake, reboot and crash handling;
- a non-dismissible parent-authentication/lock surface where required;
- preventing ordinary UI paths from disabling enforcement.

The current PCTL adapter can manage parental-control settings; it does not provide all of the above. The qlaunch/HOME integration point, process monitoring API and any Atmosphere patches must be proven on one explicit firmware/Atmosphere pair before claiming system-wide enforcement.

Prefer documented Atmosphere/Stratosphere interfaces and narrowly scoped version-specific patches. Do not ship version-blind kernel hooks. Unknown firmware must disable unsupported hooks and enter the documented safe state rather than risk a boot loop.

## 8. Concrete migration phases

### Phase 0 — freeze and reference
- Keep main unchanged and create this feature branch.
- Record baseline commit and supported firmware/Atmosphere versions.
- Pin an Atmosphere source revision and identify its official sysmodule template.
- Check license compatibility before copying build files or code.

### Phase 1 — buildable empty sysmodule
- Add sysmodule build target using the pinned Stratosphere template.
- Add module metadata, package layout and minimal start/stop entry point.
- Add an independent CI job; do not remove the NRO job yet.
- Acceptance: installable artifact, starts independently of hbmenu/Borealis, clean shutdown and reboot.

### Phase 2 — lifecycle and local IPC
- Add versioned local IPC for health, status and policy revision.
- Add suspend/resume, watchdog, bounded workers and clean shutdown.
- Acceptance: IPC health queries work; repeated start/stop and sleep/wake do not leak handles or threads.

### Phase 3 — network and server protocol
- Add TLS REST, per-device credential provisioning, WSS, reconnect/backoff, heartbeat and bounded RAM event queue.
- Bound JSON body size, nesting, strings and arrays.
- Acceptance: authenticated connection, certificate failure rejection, WSS reconnect, REST reconciliation and secret-free logs.

### Phase 4 — policy engine and RAM cache
- Add strict schema validation, immutable snapshots, revision checks and atomic replacement.
- Acceptance: malformed/stale policy rejected; network loss retains last policy; reconnect catches up; restart offline enters restricted bootstrap.

### Phase 5 — PCTL adapter
- Wrap existing PCTL operations behind PctlController.
- Preserve firmware checks and exact Result handling.
- Separate observation from privileged writes. Never expose destructive reset/unpair calls as remote commands.
- Acceptance: supported playtime/bedtime policies map to verified PCTL calls on each declared firmware.

### Phase 6 — full monitoring and enforcement
- Prove foreground title observation and qlaunch/HOME integration on hardware.
- Implement monotonic playtime accounting, title restrictions, quiet hours and lock/re-auth surface only after feasibility passes.
- Acceptance: cannot bypass through HOME, app exit, user switch, sleep/wake or reboot; crash recovery and uninstall are tested.

### Phase 7 — retire the legacy UI
- Keep the old NRO as a diagnostic/recovery utility until replacement workflows exist.
- Only then remove Borealis, activities, views, XML resources and NRO packaging from the product path.
- Update README, install guide, compatibility matrix and release assets.

## 9. CI and definition of done

Keep a temporary legacy NRO job. Add a separate sysmodule build job using the pinned Atmosphere template. Host tests cover policy parsing, revision logic, schedules, event deduplication and reconnect state. Device tests are mandatory for PCTL and system integration; a host build is not evidence of working enforcement.

The product is a headless sysmodule only when it builds as an installable Stratosphere module, starts without hbmenu/Borealis, authenticates to the server over verified TLS, uses REST+WSS, retains the last verified policy in RAM during network loss, follows restricted bootstrap after restart offline, maps PCTL correctly, and survives HOME/sleep/wake/user changes/reboot with a tested recovery path.

Until system-wide bypass resistance is demonstrated on hardware, call it a resident sysmodule prototype, not complete system-wide parental control.
