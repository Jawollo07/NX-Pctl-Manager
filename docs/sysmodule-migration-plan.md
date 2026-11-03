# Headless Atmosphere Sysmodule Migration Plan

Status: Phase 0 reference pinned; Phase 1 build target committed; Phase 2 runtime and read-only IPC scaffold added; Phase 4 policy model, validation and RAM snapshot store added as a host-tested foundation. Policy delivery, authentication, enforcement and hardware validation remain unimplemented.

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
- CMakeLists.txt, Makefile and the original GitHub Actions workflow package an NRO/ZIP, not an Atmosphere sysmodule.

Do not relabel the NRO as a sysmodule. A Stratosphere module needs its own entry point, module metadata, package layout, lifecycle, service registration, resource limits and installation method.

## 3. Pinned Atmosphere reference

The initial build glue is aligned to the official Atmosphere source at:

- Repository: https://github.com/Atmosphere-NX/Atmosphere
- Commit: `6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8`
- Official sysmodule reference: `stratosphere/TioServer`
- Official build rules: `stratosphere/TioServer/system_module.mk` and `libraries/config/templates/stratosphere.mk`
- The repository is GPL-2.0; the Atmosphere/libstratosphere build system is GPL-2.0, so the project license is compatible.

The commit is pinned in `sysmodule/README.md` and the dedicated CI workflow. It is a build reference, not yet a claim that every Switch firmware version is supported. The target console's firmware and Atmosphere release still need to be declared before device testing.

## 4. Target repository layout

Migrate additively. Keep the existing NRO build working on main until the sysmodule is independently buildable.

    sysmodule/
      Makefile
      system_module.mk
      nx_pctl_manager.json
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
      README.md
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

The current NPDM title ID `0x010000000000D7A1` is a development placeholder only. It is not registered or hardware-validated. Do not install the artifact until title ID allocation, permissions and boot recovery have been reviewed. Never guess kernel hooks or service names.

## 5. Runtime design

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

## 6. Proposed server protocol

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

## 7. Policy and security rules

A policy needs at minimum:
- schema version, device ID, monotonically increasing revision;
- issued-at, not-after and server time;
- enabled state and explicit startup/offline behaviour;
- global and weekday play-time budgets;
- quiet hours and timezone semantics;
- allowed/blocked title identifiers and restriction mode;
- parent-authentication requirements.

Reject unknown critical schema versions, invalid bounds, stale revisions, cross-device replay and malformed input. Use monotonic time for elapsed playtime; use server time only for wall-clock schedules, with explicit clock uncertainty handling. TLS validation is mandatory. Never log credentials, tokens, PINs, TOTP secrets or authentication payloads.

### Policy model implementation boundary

`protocol/policy.hpp` implements schema/protocol version 1 as a fixed-size C++ model. It includes device binding, monotonic revision, issue/expiry/server timestamps, enabled state, parent-authentication requirement, offline behavior, fixed UTC offset, global and weekday budgets, quiet hours, and up to 64 title allow/block rules. Validation rejects unsupported versions, device mismatch, stale revisions, invalid time bounds/clock skew, expired or overlong policies, invalid limits/schedules, invalid title IDs/actions, duplicate rules, and oversized rule sets.

`PolicyStore` holds only one fixed-size snapshot in RAM. It validates a candidate while holding a short spin lock against the currently active revision and replaces the complete structure only on success. A rejected candidate cannot partially mutate the active snapshot. Host tests exercise these rules. The sysmodule exposes the active revision through its read-only status command, but no worker/network path can publish a policy yet.

A bounded fixed-buffer JSON decoder now accepts the exact schema-v1 fields and rejects unknown/duplicate fields, oversized documents, excessive nesting, unsupported escapes, malformed numbers, invalid array sizes and wrong types. It uses no heap allocation and is host-tested. The decoder is still not a trust boundary: it does not authenticate its input, verify signatures/MACs, provide authenticated transport, provision device credentials, or bootstrap trusted server time. Callers must not pass untrusted data to the store until those layers exist. Timezone support currently means a fixed UTC offset only, not IANA/DST handling. No policy is persisted.

## 8. Full system integration: feasibility gate

A resident sysmodule alone does not prove that users cannot close, bypass or kill the enforcement surface. Full scope requires supported mechanisms for:
- foreground title/process observation;
- enforcement before or immediately after a disallowed title starts;
- HOME, user changes, sleep/wake, reboot and crash handling;
- a non-dismissible parent-authentication/lock surface where required;
- preventing ordinary UI paths from disabling enforcement.

The current PCTL adapter can manage parental-control settings; it does not provide all of the above. The qlaunch/HOME integration point, process monitoring API and any Atmosphere patches must be proven on one explicit firmware/Atmosphere pair before claiming system-wide enforcement.

Prefer documented Atmosphere/Stratosphere interfaces and narrowly scoped version-specific patches. Do not ship version-blind kernel hooks. Unknown firmware must disable unsupported hooks and enter the documented safe state rather than risk a boot loop.

## 9. Concrete migration phases

### Phase 0 — freeze and reference
- [x] Keep main unchanged and create this feature branch.
- [ ] Record baseline commit and supported firmware/Atmosphere versions.
- [x] Pin an Atmosphere source revision and identify its official sysmodule build rules.
- [x] Check license compatibility before adapting build glue.
- [ ] Declare the target console firmware/Atmosphere release before hardware testing.

### Phase 1 — buildable empty sysmodule
- [x] Add an independent Stratosphere build target and minimal module entry point.
- [x] Add initial NPDM metadata and a separate CI workflow.
- [ ] Obtain a successful CI build of the NSP.
- [ ] Validate start/stop and recovery on a test console.
- Important: the current title ID is a placeholder; the package is not approved for installation.

### Phase 2 — lifecycle and local IPC
- [x] Add a versioned, read-only local HIPC endpoint for protocol version, worker state, heartbeat and policy revision.
- [x] Add a bounded worker thread, dedicated stack, atomic health counters and cooperative stop event.
- [ ] Add PSC-based suspend/resume handling only after a valid project-owned module identity/dependency registration is established. The pinned Atmosphere PmModuleId enum has no project-owned ID; gaps/reserved values must not be guessed or reused. See `sysmodule/RUNTIME.md`.
- [x] Add worker failure supervision and bounded restart attempts using `os::TryWaitThread`; stop/join the supervisor before intentional worker shutdown. The current policy is at most three restarts per module lifetime, then a terminal `Failed` state.
- [ ] Add a companion-client IPC test and verify service access on the declared firmware.
- Acceptance remains pending: IPC health queries must work; repeated start/stop and sleep/wake must not leak handles or threads.

### Phase 3 — network and server protocol
- Add TLS REST, per-device credential provisioning, WSS, reconnect/backoff, heartbeat and bounded RAM event queue.
- Bound JSON body size, nesting, strings and arrays.
- Acceptance: authenticated connection, certificate failure rejection, WSS reconnect, REST reconciliation and secret-free logs.

### Phase 4 — policy engine and RAM cache
- [x] Add a versioned fixed-size policy model and strict structural/range validation.
- [x] Bind snapshots to one device and require strictly increasing revisions.
- [x] Add a bounded RAM-only snapshot store; validate under lock and replace the complete snapshot or leave the previous one untouched.
- [x] Add bounded fixed-buffer JSON decoding; reject unknown/duplicate fields, malformed values and oversized documents.
- [ ] Add authenticated transport and policy integrity verification before publication.
- [ ] Connect worker/network lifecycle to policy publication, offline retention and restricted bootstrap.
- [x] Add pure evaluator tests for weekday budgets, quiet hours, title allow/block modes, parent authentication and offline decisions.
- [ ] Add monotonic playtime accounting and cross-midnight/day-boundary tests.
- Acceptance remains pending: malformed/stale policy rejected; network loss retains last policy; reconnect catches up; restart offline enters restricted bootstrap.

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

## 10. CI and definition of done

Keep a temporary legacy NRO job. Add a separate sysmodule build job using the pinned Atmosphere template. Host tests cover policy parsing, revision logic, schedules, event deduplication and reconnect state. Device tests are mandatory for PCTL and system integration; a host build is not evidence of working enforcement.

The product is a headless sysmodule only when it builds as an installable Stratosphere module, starts without hbmenu/Borealis, authenticates to the server over verified TLS, uses REST+WSS, retains the last verified policy in RAM during network loss, follows restricted bootstrap after restart offline, maps PCTL correctly, and survives HOME/sleep/wake/user changes/reboot with a tested recovery path.

Until system-wide bypass resistance is demonstrated on hardware, call it a resident sysmodule prototype, not complete system-wide parental control.
