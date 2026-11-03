# Sysmodule runtime

## Current runtime phase

The experimental Stratosphere target now has a resident main thread, a separate cooperative worker thread, and a read-only local HIPC service.

- `InitializeSystemModule` initializes the service-manager client, checks the pinned libstratosphere API, and creates the worker stop event.
- `Main` starts the worker, registers the `nxpctl` HIPC service, and enters the Stratosphere server loop.
- The worker uses a one-second timed event wait as its idle cadence and cooperative shutdown point. A separate supervisor checks thread termination with `os::TryWaitThread`, reaps the exited thread, and permits at most three automatic restarts per module lifetime. After the retry budget is exhausted, the service reports `WorkerState::Failed` and does not restart indefinitely.
- Finalization stops and joins the supervisor before stopping the worker, preventing an intentional shutdown from being mistaken for a worker failure.
- HIPC command 65000 (`GetRuntimeStatus`) returns protocol version, worker state, worker heartbeat, bounded worker restart count, and policy revision. The initial policy revision is zero.
- `protocol/runtime_status.hpp` is the shared, Horizon-independent wire contract used by both the NRO client and sysmodule. `protocol/worker_supervision.hpp` contains the Horizon-independent three-restart budget used by the supervisor; CI tests that budget independently. `WorkerState::Failed` is a terminal health state after the bounded restart budget is exhausted. The CMIF output layout is four `u32` values at offsets 0/4/8/12 (protocol version, worker state, heartbeat, restart count), then the aligned `u64` policy revision at offset 16. CI compiles and runs a host-side ABI test for the service name, command ID, field offsets, and structure size.
- The existing NRO performs an optional read-only status request at startup. If the service is absent, it logs the condition and continues in standalone mode.
- `FinalizeSystemModule` signals the worker, joins it, destroys its thread object, and finalizes the event.

The HIPC interface is read-only. It does not inspect running titles, change parental-control state, apply restrictions, access persistent storage, or perform network I/O. No policy is persisted on the console.

## Next implementation phases

1. Verify live HIPC session establishment and command dispatch on supported hardware/CFW; the current CI test validates the shared ABI, not the Horizon IPC transport.
2. [x] Add worker failure supervision and bounded restart attempts. [ ] Add explicit suspend/resume state transitions after the PSC identity gate is resolved.
3. Add authenticated HTTPS transport and device credential provisioning; add WSS only after the REST trust boundary and reconnect behavior are proven.
4. [x] Add bounded policy validation and in-memory policy snapshots.
5. [x] Add bounded schema-v1 JSON decoding and host tests.
6. Add authenticated message integrity verification and connect only verified full snapshots to the RAM store.
7. Implement enforcement only after the required Horizon services, permissions, firmware compatibility, and recovery behavior have been validated.

## Power-state integration gate

The pinned Atmosphere revision exposes suspend/resume notifications through `ams::psc::PmModule`. Its `PmModuleId` enum assigns named IDs to platform modules (including PCTL and other system services), but it does not provide a project-owned ID for this sysmodule. The gaps and reserved value are not safe allocation points. This project must not claim one of those IDs or register as another system service. PSC integration remains blocked until a valid module identity/dependency registration is established for the target firmware. Until then, no PSC request is acknowledged and no suspend/resume behavior is implemented.

## Safety boundary

The NPDM title ID remains provisional. Do not install or launch this artifact on a console until the title ID, service permissions, kernel capabilities, firmware compatibility, and recovery procedure have been reviewed. CI packaging does not constitute hardware validation.


## RAM-only policy model (validation stage)

`protocol/policy.hpp` now defines the fixed-size, versioned policy model and a bounded in-memory `PolicyStore`. It validates device binding, protocol/schema versions, strictly increasing revisions, trusted-server clock skew, expiry and maximum lifetime, offline behavior, fixed UTC offset, daily limits, quiet hours, and bounded/unique title rules. A candidate is validated again while holding the store lock against the currently active revision, then copied as one complete snapshot. Invalid or stale candidates leave the previous snapshot unchanged.

The store uses fixed-size memory and performs no allocation, file I/O, database access, or network access. The HIPC status endpoint reports the store's active revision (zero until a future authenticated policy-delivery path publishes a snapshot). The model is not yet populated by the worker and does not enforce restrictions.

The host test covers validation failures, device/revision binding, full-snapshot replacement, and preservation of the previous snapshot after rejection. This stage does **not** parse JSON, verify signatures/MACs, authenticate a server, or establish that a received policy is trustworthy; those checks must precede calls to `PolicyStore::TryReplace`. The fixed UTC offset is not an IANA timezone/DST implementation.


## Bounded policy JSON decoding

`protocol/policy_json.hpp` adds a fixed-buffer parser for schema v1. It accepts documents up to 16 KiB, uses a fixed token array, bounds nesting and string length, rejects unknown and duplicate root fields, requires every schema field, checks array lengths and integer ranges, and rejects malformed numbers and unsupported string escapes. No heap allocation is performed.

The accepted schema uses printable ASCII strings without escapes, integer-only numeric fields, numeric enum values, exactly seven weekday limits, and title-rule objects containing exactly `title_id` and `action`. This deliberately strict subset must match the server serializer. JSON decoding is followed by the existing policy validator through `DecodeAndValidate`; only a fully decoded and structurally valid candidate is returned.

CI now exercises valid decoding plus malformed/oversized input, unknown/duplicate keys, escapes, fractional values, incorrect arrays and invalid enum values. The decoder does not authenticate its input, verify signatures, or publish to the active store. The worker must not call `PolicyStore::TryReplace` until authenticated transport and message integrity are implemented.


## Deterministic policy evaluation (not enforcement)

`protocol/policy_evaluator.hpp` now evaluates a supplied immutable policy and
context without Horizon calls or side effects. It covers parent-authentication
gating, offline behavior, explicit title blocks, allow-all versus allow-list-only
title modes, fixed-offset weekday budgets and quiet hours including intervals
that cross midnight. Explicit title allows do not override global quiet hours,
daily budgets, parent authentication or offline restrictions.

The evaluator is host-tested. It is not yet called by the sysmodule worker and
does not observe title launches, track playtime, mutate PCTL state, or block
anything on a console. Monotonic accounting and a verified system integration
point remain separate work.
