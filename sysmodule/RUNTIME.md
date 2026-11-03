# Sysmodule runtime

## Current runtime phase

The experimental Stratosphere target now has a resident main thread, a separate cooperative worker thread, and a local HIPC status and enrollment-provisioning service.

- `InitializeSystemModule` initializes the service-manager client, checks the pinned libstratosphere API, and creates the worker stop event.
- `Main` starts the worker, registers the `nxpctl` HIPC service, and enters the Stratosphere server loop.
- The worker uses a dedicated 32 KiB stack and a one-second timed event wait as its idle cadence and cooperative shutdown point. The larger stack accommodates the bounded schema-v1 parser and network call frames. A separate supervisor checks thread termination with `os::TryWaitThread`, reaps the exited thread, and permits at most three automatic restarts per module lifetime. After the retry budget is exhausted, the service reports `WorkerState::Failed` and does not restart indefinitely.
- Finalization stops and joins the supervisor before stopping the worker, preventing an intentional shutdown from being mistaken for a worker failure.
- HIPC command 65000 (`GetRuntimeStatus`) returns protocol version, worker state, worker heartbeat, bounded worker restart count, and policy revision. Command 65001 (`ConfigureEnrollment`) accepts one fixed-size, validated host/device-ID/enrollment-code buffer. It is not a general configuration or policy-write interface.
- `protocol/runtime_status.hpp` is the shared, Horizon-independent wire contract used by both the NRO client and sysmodule. `protocol/worker_supervision.hpp` contains the Horizon-independent three-restart budget used by the supervisor; CI tests that budget independently. `WorkerState::Failed` is a terminal health state after the bounded restart budget is exhausted. The CMIF output layout is four `u32` values at offsets 0/4/8/12 (protocol version, worker state, heartbeat, restart count), then the aligned `u64` policy revision at offset 16.
- `protocol/http_response.hpp` now provides a bounded, allocation-free HTTP/1.0/1.1 response parser. It requires exactly one decimal `Content-Length`, rejects chunked/other transfer encodings, rejects compressed bodies, duplicate lengths, malformed CRLF framing, truncated bodies and trailing bytes, and caps headers/body at 8 KiB/96 KiB. It returns a view into the caller-owned receive buffer; it does not perform socket I/O or authenticate the peer.
- The existing NRO performs an optional read-only status request at startup. If the service is absent, it logs the condition and continues in standalone mode.
- `FinalizeSystemModule` signals the worker, joins it, destroys its thread object, and finalizes the event.

The HIPC interface exposes read-only diagnostics plus a single enrollment bootstrap operation. It does not inspect running titles, change parental-control state, apply restrictions or access persistent storage. The NRO offers an enrollment screen and passes the API hostname, server-assigned device ID and one-time code to the sysmodule. The worker then performs encrypted enrollment, exchanges the returned device credential for a short-lived token, fetches and validates policy snapshots, sends heartbeats and acknowledges accepted revisions.

## Next implementation phases

1. Verify live HIPC session establishment and command dispatch on supported hardware/CFW; the current CI test validates the shared ABI, not the Horizon IPC transport.
2. [x] Add worker failure supervision and bounded restart attempts. [ ] Add explicit suspend/resume state transitions after the PSC identity gate is resolved.
3. [x] Implement Horizon HTTP transport, local enrollment provisioning, encrypted enrollment/authentication, token refresh, heartbeat and policy retrieval/acknowledgement. HTTP is the selected device transport. [ ] Add protected credential persistence/recovery so an enrolled device survives a sysmodule restart without revocation and re-enrollment.
4. [x] Add bounded policy validation and in-memory policy snapshots.
5. [x] Add bounded schema-v1 JSON decoding and host tests.
6. [x] Add bounded HTTP response framing parser and host tests. [ ] Add authenticated message integrity verification and connect only verified full snapshots to the RAM store.
7. Implement enforcement only after the required Horizon services, permissions, firmware compatibility, and recovery behavior have been validated.

## Power-state integration gate

The pinned Atmosphere revision exposes suspend/resume notifications through `ams::psc::PmModule`. Its `PmModuleId` enum assigns named IDs to platform modules (including PCTL and other system services), but it does not provide a project-owned ID for this sysmodule. The gaps and reserved value are not safe allocation points. This project must not claim one of those IDs or register as another system service. PSC integration remains blocked until a valid module identity/dependency registration is established for the target firmware. Until then, no PSC request is acknowledged and no suspend/resume behavior is implemented.

## Safety boundary

The NPDM title ID remains provisional. Do not install or launch this artifact on a console until the title ID, service permissions, kernel capabilities, firmware compatibility, and recovery procedure have been reviewed. CI packaging does not constitute hardware validation.

## RAM-only policy model (validation stage)

`protocol/policy.hpp` defines the fixed-size, versioned policy model and a bounded in-memory `PolicyStore`. It validates device binding, protocol/schema versions, strictly increasing revisions, trusted-server clock skew, expiry and maximum lifetime, offline behavior, fixed UTC offset, daily limits, quiet hours, and bounded/unique title rules. A candidate is validated again while holding the store lock against the currently active revision, then copied as one complete snapshot. Invalid or stale candidates leave the previous snapshot unchanged.

The store uses fixed-size memory and performs no allocation, file I/O, database access, or network access. The HIPC status endpoint reports the store's active revision. The secure sync worker now feeds it only after encrypted response verification and schema/clock/device/revision validation. The model still does not enforce restrictions.

## Bounded policy JSON decoding

`protocol/policy_json.hpp` adds a fixed-buffer parser for schema v1. It accepts documents up to 16 KiB, uses a fixed token array, bounds nesting and string length, rejects unknown and duplicate root fields, requires every schema field, checks array lengths and integer ranges, and rejects malformed numbers and unsupported string escapes. No heap allocation is performed.

The accepted schema uses printable ASCII strings without escapes, integer-only numeric fields, numeric enum values, exactly seven weekday limits, and title-rule objects containing exactly `title_id` and `action`. This deliberately strict subset must match the server serializer. JSON decoding is followed by the existing policy validator through `DecodeAndValidate`; only a fully decoded and structurally valid candidate is returned. The decoder does not authenticate its input; the secure sync worker verifies the AEAD envelope before calling it and publishes only a fully validated snapshot.

## Deterministic policy evaluation (not enforcement)

`protocol/policy_evaluator.hpp` evaluates a supplied immutable policy and context without Horizon calls or side effects. It covers parent-authentication gating, offline behavior, explicit title blocks, allow-all versus allow-list-only title modes, fixed-offset weekday budgets and quiet hours including intervals that cross midnight. Explicit title allows do not override global quiet hours, daily budgets, parent authentication or offline restrictions.

The evaluator is host-tested. It is not yet called by the sysmodule worker and does not observe title launches, track playtime, mutate PCTL state, or block anything on a console. Monotonic accounting and a verified system integration point remain separate work.


## HTTP transport and secure envelope client

`protocol/http_request.hpp` and `protocol/http_response.hpp` provide
allocation-free HTTP framing with 96 KiB body limits, strict Content-Length
handling, and rejection of chunked/compressed responses.

`source/http_transport.cpp` initializes Atmosphere's system BSD client,
resolves IPv4 hostnames through the platform resolver, and performs nonblocking
TCP connects with a five-second timeout. Send and receive waits are capped at
eight seconds per wait. Connections are closed after each request. DNS lookup
is synchronous and does not yet have a project-enforced deadline.

`source/secure_http_client.cpp` builds the exact v1 envelope, derives the
AES-128 key and SHA-256 key selector, generates a fresh 63-bit request ID and
96-bit nonce, binds request/response direction and path in AAD, and verifies
the response selector, echoed request ID and GCM tag before exposing plaintext.
The AES-GCM NIST vector and backend-matched HKDF vector are run before the first
secure POST. Large transport buffers are static BSS allocations, not worker
stack allocations.

The callable client is not yet invoked by the worker: the trusted enrollment
code/API-host provisioning channel and device credential lifecycle are not
implemented. Do not expose the backend publicly until cross-language envelope
fixtures, provisioning and real-device validation are complete.

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
nonblocking with a five-second timeout; send/receive waits are capped at eight
seconds each. DNS resolution is synchronous and does not yet have a project
deadline.

Cloudflare can proxy ciphertext but still sees IP addresses, hostname, URL
paths, headers, packet sizes and timing and can block or delay traffic. API
paths must bypass caching and body transformations; do not apply redirects,
Access login or browser challenges. Keep the origin private.

## Network service permissions and implementation audit

The pinned Atmosphere socket backend initializes libnx BSD in system-client
mode. The sysmodule uses `bsd:s` for sockets and `sfdnsres` for hostname
resolution. It does not request `ssl` and does not perform a TLS handshake.
NIFM is intentionally not requested: the audited socket initialization path
does not call `nifmInitialize`. The NPDM syscall list includes
`svcCreateTransferMemory` (`0x15`) because the BSD client shares its
configured socket memory pool with `bsd:s`.

The transport and secure envelope client are now implemented, but the worker
does not invoke them yet. Trusted API-host and one-time enrollment-code
provisioning, device credential lifecycle, token refresh, cross-language
envelope fixtures and real-device validation remain required before public
deployment. The policy store is not populated from the network and no
restrictions are enforced.
