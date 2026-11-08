# Sysmodule runtime

## Current runtime phase

The experimental Stratosphere target now has a resident main thread, a separate cooperative network worker, a dedicated global playtime monitor, a PGL process-start event monitor, and a local HIPC status and enrollment-provisioning service.

- `InitializeSystemModule` initializes the service-manager client, checks the pinned libstratosphere API, creates the worker stop event, and attempts to mount this sysmodule’s own device save data before the worker starts.
- `Main` starts the worker, registers the `nxpctl` HIPC service, and enters the Stratosphere server loop.
- The worker uses a dedicated 32 KiB stack and a one-second timed event wait as its idle cadence and cooperative shutdown point. The larger stack accommodates the bounded schema-v1 parser and network call frames. A separate supervisor checks thread termination with `os::TryWaitThread`, reaps the exited thread, and permits at most three automatic restarts per module lifetime. After the retry budget is exhausted, the service reports `WorkerState::Failed` and does not restart indefinitely.
- Finalization stops and joins the supervisor before stopping the worker, preventing an intentional shutdown from being mistaken for a worker failure.
- HIPC command 65000 (`GetRuntimeStatus`) returns protocol version, worker state, worker heartbeat, bounded worker restart count, and policy revision. Command 65001 (`ConfigureEnrollment`) accepts one fixed-size, validated host/device-ID/enrollment-code buffer. Command 65002 (`GetUsageStatus`) returns monitor state, global active seconds, and rounded daily minutes. Command 65003 (`GetPolicyDecision`) returns whether a trusted-time global policy decision is available, its allow/deny result, and the `DecisionReason` numeric value. Command 65004 (`GetEnforcementStatus`) returns the PGL enforcement state, cumulative process-termination attempts, and process-start event-monitor state (0=stopped, 1=active, 2=waiting/retrying). The monitor derives weekday and minute-of-day from the authenticated server-time anchor plus monotonic elapsed time; it never consults the console wall clock. The evaluator receives offline state from authenticated backend-contact freshness (120-second grace period; unknown after boot is treated as offline). Parent-authentication state is obtained from the backend over the encrypted `parent-auth` operation and expires against the trusted server-time anchor. PGL enforcement applies the resulting global allow/deny decision. Monitor state `0` means unavailable/stopped, `1` means monitoring, and `2` means persistent usage data is corrupt and the counter has failed closed. These are not general configuration or policy-write interfaces.
- `protocol/runtime_status.hpp` is the shared, Horizon-independent wire contract for the sysmodule and any separately built HIPC management client. `protocol/worker_supervision.hpp` contains the Horizon-independent three-restart budget used by the supervisor; CI tests that budget independently. `WorkerState::Failed` is a terminal health state after the bounded restart budget is exhausted. The CMIF output layout is four `u32` values at offsets 0/4/8/12 (protocol version, worker state, heartbeat, restart count), then the aligned `u64` policy revision at offset 16.
- `protocol/http_response.hpp` now provides a bounded, allocation-free HTTP/1.0/1.1 response parser. It requires exactly one decimal `Content-Length`, rejects chunked/other transfer encodings, rejects compressed bodies, duplicate lengths, malformed CRLF framing, truncated bodies and trailing bytes, and caps headers/body at 8 KiB/96 KiB. It returns a view into the caller-owned receive buffer; it does not perform socket I/O or authenticate the peer.
- `InitializeSystemModule` attempts PGL initialization only on Horizon 10.0.0 or later; the monitor retries a failed connection on its one-second cadence and reports enforcement error state `3` while PGL is unavailable. `Main` starts both PGL monitors independently from the network worker, even if the initial PGL connection failed. The event monitor retries observer creation if needed and uses a timed wait so shutdown remains cooperative. `FinalizeSystemModule` stops and joins both monitors before finalizing PGL, then stops the other threads and finalizes their events.

The HIPC interface exposes diagnostics, a global usage snapshot, enforcement status (including event-monitor health), authenticated connectivity status, and a single enrollment bootstrap operation. Command 65005 reports connection state and the age of the last authenticated server response. The monitor obtains the current application PID only for the immediate PGL termination call; it never stores the PID or inspects title IDs. No parental-control service is used. Credential persistence uses this sysmodule’s own device save data. The removed NRO is not part of this headless branch. First-time enrollment currently requires a separately built trusted HIPC client; no such client is shipped in this repository, so enrollment remains an end-user integration blocker. The worker then performs encrypted enrollment, exchanges the returned device credential for a short-lived token, fetches and validates policy snapshots, sends heartbeats and acknowledges accepted revisions.

## Next implementation phases

1. Verify live HIPC session establishment and command dispatch on supported hardware/CFW; the current CI test validates the shared ABI, not the Horizon IPC transport.
2. [x] Add worker failure supervision and bounded restart attempts. [ ] Add explicit suspend/resume state transitions after the PSC identity gate is resolved.
3. [x] Implement Horizon HTTP transport, local enrollment provisioning, encrypted enrollment/authentication, token refresh, heartbeat, policy retrieval/acknowledgement and protected credential persistence/recovery. HTTP is the selected device transport.
4. [x] Add bounded policy validation and in-memory policy snapshots.
5. [x] Add bounded schema-v1 JSON decoding and host tests.
6. [x] Add bounded HTTP response framing parser and host tests. [x] Add authenticated message integrity verification and connect only verified full snapshots to the RAM store.
7. [x] Add PCTL-free global enforcement through the pinned PGL TerminateProcess API. [x] Add a PGL process-start event observer to enforce cached global deny decisions immediately when a new application process is reported. Hardware validation, suspend/resume accounting and a tested recovery path remain outstanding.

## PGL-based global enforcement (PCTL-free)

The pinned Atmosphere/libstratosphere revision
(`6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8`) exposes both
`ams::pgl::GetRunningApplicationProcessId` and
`ams::pgl::TerminateProcess(os::ProcessId)` in
`pgl/pgl_shell_api.hpp`. The implementation delegates to the Horizon PGL
service. PGL command 1 forwards to `pm:shell->TerminateProcess`; command 5
returns the current application PID. Atmosphère's own crash-report module
uses this exact PGL termination wrapper on Horizon 10.0.0 and later. This
project uses PGL only; it does not initialize, query, or modify Nintendo PCTL.

A dedicated one-second monitor thread queries PGL independently of the network
worker. A successful query means an application is active. PGL's documented
no-running-application result (`0x6E4`) means idle; any other query failure is
unknown and is reported as an enforcement error rather than treated as idle.
The PID is used only as a transient argument to `pgl::TerminateProcess`; it is
never persisted, indexed, or associated with a title ID. Usage accounting
continues to receive only an active/inactive boolean, so all applications share
one global daily counter.

When a verified policy allows use, enforcement state is `1`. When a verified
policy denies use, the monitor calls `pgl::TerminateProcess` for the running
application and records state `2` on success or `3` on failure. If an enrolled
device has no usable verified policy/trusted clock, it fails closed and
terminates running applications; state `4` reports this condition. An
unenrolled device is not locked by an absent policy. HIPC command 65004
(`GetEnforcementStatus`) exposes the state and cumulative termination-attempt
count and the event observer's health state. Command 65003 continues to expose the policy decision.

This is process termination, not PCTL suspension and not a whole-console lock.
The one-second monitor remains the accounting and recovery fallback. A separate
PGL shell-event observer drains queued `pm::ProcessEventInfo` records and reacts
to `Started` events. It compares the event PID transiently with
`GetRunningApplicationProcessId` before terminating, so non-application process
notifications are not treated as games. It does not retain the PID or inspect a
title ID. The observer queue only reports processes tracked by PGL; the polling
monitor remains necessary for startup races, missed events, and existing
applications. When the observer is active, ordinary relaunches can be handled
on the process-start notification rather than waiting for the next poll, but
this timing and event coverage still require hardware validation. HOME, system settings, recovery mode and
control of the boot chain are outside this application-process enforcement
boundary. A person able to disable or replace the sysmodule/CFW can bypass it.
No absolute tamper-proof guarantee is claimed.

The trusted local-day key comes from the authenticated policy `server_time`
and the policy's fixed UTC offset. The server timestamp is anchored to
Atmosphère's monotonic system tick and advanced only by monotonic elapsed time;
the device wall clock is not used. The anchor is valid only until the policy's
`not_after` time. An enrolled device without a valid anchor is fail-closed for
application execution, rather than inventing usage or silently allowing use.

The tracker restores its global daily total from two alternating AES-128-GCM
authenticated records in this sysmodule's own device save data. A device-derived
SPL key encrypts the snapshot; the plaintext total and key are not written to
storage. Each record includes a monotonically increasing sequence, trusted
local-day key and active seconds. Writes replace the older slot and flush the
complete record, leaving the newest valid slot available if power is lost
mid-write. On boot, the newest authenticated snapshot is selected. A matching
trusted day restores its total; a later trusted day starts at zero; a trusted
clock earlier than the stored day is rejected rather than erasing usage.
Snapshots are attempted at most once per minute while dirty and are force-flushed
on orderly module shutdown. A sudden power loss can therefore lose up to about
one minute of newly accumulated usage. If save-data mounting or key derivation
fails, monitoring remains RAM-only for that session and persistence is not
claimed. Failed writes leave the newest valid slot intact and are retried at
the next persistence interval. If two authenticated records have the same
sequence but different totals or day keys, the snapshot set is treated as
ambiguous and rejected. If both existing slots are present but fail
authentication/validation, the monitor reports storage error state `2` and
refuses to overwrite them or silently reset the counter. Started minutes round
up, forward local-day transitions reset the bucket, gaps longer than two
seconds are ignored, and backward clock corrections do not erase an accumulated
counter. HIPC command 65002 exposes monitor state, global active seconds and
daily minutes.

## Power-state integration gate

The pinned Atmosphere revision exposes suspend/resume notifications through `ams::psc::PmModule`. Its `PmModuleId` enum assigns named IDs to platform modules and system services, but it does not provide a project-owned ID for this sysmodule. The gaps and reserved value are not safe allocation points. This project must not claim one of those IDs or register as another system service. PSC integration remains blocked until a valid module identity/dependency registration is established for the target firmware. Until then, no PSC request is acknowledged and no suspend/resume behavior is implemented.

## Safety boundary

The NPDM title ID is currently configured as `0x420000000A3F1E7C`, but is not yet confirmed as registered or hardware-validated. Do not install or launch this artifact on a console until the title ID, service permissions, kernel capabilities, firmware compatibility, and recovery procedure have been reviewed. CI packaging does not constitute hardware validation.

## RAM-only policy model (validation stage)

`protocol/policy.hpp` defines the fixed-size, versioned policy model and a bounded in-memory `PolicyStore`. It validates device binding, protocol/schema versions, strictly increasing revisions, trusted-server clock skew, expiry and maximum lifetime, offline behavior, fixed UTC offset, daily limits, quiet hours, and global offline behavior. A candidate is validated again while holding the store lock against the currently active revision, then copied as one complete snapshot. Invalid or stale candidates leave the previous snapshot unchanged.

The store uses fixed-size memory and performs no allocation, file I/O, database access, or network access. The HIPC status endpoint reports the store's active revision. The secure sync worker now feeds it only after encrypted response verification and schema/clock/device/revision validation. The runtime enforces the resulting global decision through PGL.

## Bounded policy JSON decoding

`protocol/policy_json.hpp` adds a fixed-buffer parser for schema v1. It accepts documents up to 16 KiB, uses a fixed token array, bounds nesting and string length, rejects unknown and duplicate root fields, requires every schema field, checks array lengths and integer ranges, and rejects malformed numbers and unsupported string escapes. No heap allocation is performed.

The accepted schema uses printable ASCII strings without escapes, integer-only numeric fields, numeric enum values, exactly seven weekday limits, and rejects all title-specific fields as unknown. This deliberately strict subset must match the server serializer. JSON decoding is followed by the existing policy validator through `DecodeAndValidate`; only a fully decoded and structurally valid candidate is returned. The decoder does not authenticate its input; the secure sync worker verifies the AEAD envelope before calling it and publishes only a fully validated snapshot.

## Global daily usage accounting

`protocol/usage_tracker.hpp` provides a host-testable, allocation-free global
daily console counter. It consumes only an application-active boolean,
monotonic seconds and a trusted local-day key. It does not receive, store or
distinguish title IDs. The dedicated PGL monitor thread feeds it independently
of network synchronization. Switching applications does not interrupt the
counter. Time is counted only while PGL reports an active application;
HOME-menu/overlay behavior requires hardware validation. Started minutes
round up, forward day transitions reset the daily bucket, backward clock corrections do not
erase accumulated time, and observation gaps over two seconds are ignored.
The counter is persisted as an encrypted daily snapshot. It supplies the policy evaluator; the separate runtime enforcement path consumes the evaluator's global decision and applies it through PGL.

`protocol/trusted_clock.hpp` derives the local day from an authenticated
policy server-time anchor, monotonic elapsed seconds and the policy's fixed
UTC offset. It rejects invalid, expired or regressed anchors. Host tests cover
UTC and offset day boundaries, expiry and monotonic regression.

## Deterministic global policy evaluation

`protocol/policy_evaluator.hpp` evaluates a supplied immutable policy and context without Horizon calls or side effects. It covers parent-authentication gating, offline behavior, global daily-limit decisions, fixed-offset weekday budgets and quiet hours including intervals that cross midnight. All decisions apply globally to the console and never inspect or distinguish individual titles.

The evaluator is host-tested and is called by the usage monitor to publish a diagnostic global allow/deny decision through HIPC command 65003. Its offline input is wired to authenticated backend-contact freshness: a fully authenticated server response keeps the device online for 120 seconds, after which it is offline; unknown connectivity after boot is treated as offline. The parent-authentication input is wired to the backend-issued short-lived grant. The TOTP secret and verification remain backend-side; the Switch receives only the authenticated grant state and expiry. The evaluator itself remains pure; the runtime consumes its decision and uses PGL to terminate the active application when globally denied. It does not lock HOME/qlaunch or block system menus. See [offline behavior](../docs/offline-behavior.md).

## HTTP transport and secure envelope client

`protocol/http_request.hpp` and `protocol/http_response.hpp` provide
allocation-free HTTP framing with 96 KiB body limits, strict Content-Length
handling, and rejection of chunked/compressed responses.

`sysmodule/source/http_transport.cpp` initializes Atmosphere's system BSD client,
resolves IPv4 hostnames through the platform resolver on one dedicated,
single-flight resolver thread. The HTTP worker waits at most five seconds for
DNS. If the platform resolver has not returned, the worker receives a timeout;
the resolver thread is not forcibly cancelled and retains its static state
until it exits. Further lookups fail fast while that thread remains active,
and a completed late result is discarded before the next lookup. TCP connects
are nonblocking with a five-second timeout. Send and receive waits are capped
at eight seconds per wait. Connections are closed after each request.

`sysmodule/source/secure_http_client.cpp` builds the exact v1 envelope, derives the
AES-128 key and SHA-256 key selector, generates a fresh 63-bit request ID and
96-bit nonce, binds request/response direction and path in AAD, and verifies
the response selector, echoed request ID and GCM tag before exposing
plaintext. The AES-GCM NIST vector and backend-matched HKDF vector are run
before the first secure POST. Large transport buffers are static BSS
allocations, not worker stack allocations.

A separately built trusted provisioning client must supply the API hostname, device ID and one-time enrollment code through HIPC command 65001. No NRO, Borealis dependency or provisioning UI is shipped in this branch; the provisioning client is not yet included.
The worker invokes the secure client to enroll,
authenticate, refresh its short-lived token, send heartbeats, retrieve and
validate policy snapshots, publish accepted snapshots to the in-memory policy store,
and acknowledge their revisions. The enrollment code is erased after a
successful enrollment. The API host, device ID and long-lived device credential
are stored in two versioned AES-128-GCM records in this sysmodule's own device
save data. The encryption key is derived through SPL from an application-owned
key source and the console device key ladder; neither the key nor plaintext
credential is written to storage. Alternating records preserve the previous
valid record if a write is interrupted. On restart, the newest authenticated
record is restored and the worker obtains a fresh short-lived access token.
Access tokens and enrollment codes are never persisted. If save-data mounting
or key derivation fails, the current session can still operate in RAM, but
persistence is not claimed. Do not expose the backend publicly until
cross-language fixtures and real-device validation are complete.

## HTTP and Cloudflare deployment decision

The device API uses HTTP POST with AES-128-GCM-encrypted request and response
bodies. The FastAPI backend exposes `POST /v1/secure/{device_id}/{operation}`;
legacy plaintext device routes are disabled by default. The outer envelope
contains the version, 63-bit random request identifier, SHA-256 key selector,
96-bit nonce and ciphertext-plus-tag. The request ID is bound into AAD and
echoed by the response. Completed duplicate requests are replayed from the
backend's cached encrypted response; an in-flight duplicate receives 409.

The Switch-side client derives the same AES key as the backend, creates the
exact request AAD, sends the encrypted envelope over HTTP, and verifies the
response key selector, request ID and GCM tag before returning plaintext.
The first secure POST is gated by NIST AES-GCM and backend-matched HKDF
self-tests. Transport buffers are static and bounded. TCP connect is
nonblocking with a five-second timeout; send/receive waits are capped at
eight seconds each. DNS has a five-second caller deadline and a single-flight
resolver thread; a timed-out resolver is allowed to finish safely in the
background and prevents overlapping resolver calls.

Cloudflare can proxy ciphertext but still sees IP addresses, hostname, URL
paths, headers, packet sizes and timing and can block or delay traffic. API
paths must bypass caching and body transformations; do not apply redirects,
Access login or browser challenges. Keep the origin private.

## Network service permissions and implementation audit

The pinned Atmosphere socket backend initializes libnx BSD in system-client
mode. The sysmodule uses `bsd:s` for sockets and `sfdnsres` for hostname
resolution. Credential persistence additionally uses `fsp-srv` to mount its
own device save data and `spl:crypto` to derive a console-bound storage key.
The NPDM filesystem capability mask grants only `DeviceSaveData` and
`CreateOwnSaveData`; it does not grant SD-card, BIS or other-save-data access.
It does not request `ssl` and does not perform a TLS handshake.
NIFM is intentionally not requested: the audited socket initialization path
does not call `nifmInitialize`. The NPDM syscall list includes
`svcCreateTransferMemory` (`0x15`) because the BSD client shares its
configured socket memory pool with `bsd:s`.

The worker now invokes the transport and secure envelope client after
local provisioning. Enrollment, authentication, token refresh, heartbeat,
policy retrieval, policy validation/publication and policy acknowledgement
are implemented, including protected device-credential persistence. The API
host, device ID and long-lived credential are stored in alternating
AES-GCM-authenticated records in this sysmodule's own device save data. After
a restart, the newest valid record is restored and a fresh short-lived access
token is obtained. Cross-language fixture coverage and real-device
validation remain required before public deployment. Global restrictions are
currently enforced by the PGL process-termination path described above; actual
console behavior still requires hardware validation.
