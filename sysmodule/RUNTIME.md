# Sysmodule runtime

## Current runtime phase

The experimental Stratosphere target now has a resident main thread, a separate cooperative worker thread, and a read-only local HIPC service.

- `InitializeSystemModule` initializes the service-manager client, checks the pinned libstratosphere API, and creates the worker stop event.
- `Main` starts the worker, registers the `nxpctl` HIPC service, and enters the Stratosphere server loop.
- The worker uses a one-second timed event wait as its idle cadence and cooperative shutdown point. A separate supervisor checks thread termination with `os::TryWaitThread`, reaps the exited thread, and permits at most three automatic restarts per module lifetime. After the retry budget is exhausted, the service reports `WorkerState::Failed` and does not restart indefinitely.
- Finalization stops and joins the supervisor before stopping the worker, preventing an intentional shutdown from being mistaken for a worker failure.
- HIPC command 65000 (`GetRuntimeStatus`) returns protocol version, worker state, worker heartbeat, bounded worker restart count, and policy revision. The initial policy revision is zero.
- `protocol/runtime_status.hpp` is the shared, Horizon-independent wire contract used by both the NRO client and sysmodule. `protocol/worker_supervision.hpp` contains the Horizon-independent three-restart budget used by the supervisor; CI tests that budget independently. `WorkerState::Failed` is a terminal health state after the bounded restart budget is exhausted. The CMIF output layout is four `u32` values at offsets 0/4/8/12 (protocol version, worker state, heartbeat, restart count), then the aligned `u64` policy revision at offset 16.
- `protocol/http_response.hpp` now provides a bounded, allocation-free HTTP/1.0/1.1 response parser. It requires exactly one decimal `Content-Length`, rejects chunked/other transfer encodings, rejects compressed bodies, duplicate lengths, malformed CRLF framing, truncated bodies and trailing bytes, and caps headers/body at 8 KiB/96 KiB. It returns a view into the caller-owned receive buffer; it does not perform socket I/O or authenticate the peer.
- The existing NRO performs an optional read-only status request at startup. If the service is absent, it logs the condition and continues in standalone mode.
- `FinalizeSystemModule` signals the worker, joins it, destroys its thread object, and finalizes the event.

The HIPC interface is read-only. It does not inspect running titles, change parental-control state, apply restrictions or access persistent storage. The secure HTTP client is implemented as a callable transport primitive, but no host/device credential is provisioned and the worker does not yet invoke API operations.

## Next implementation phases

1. Verify live HIPC session establishment and command dispatch on supported hardware/CFW; the current CI test validates the shared ABI, not the Horizon IPC transport.
2. [x] Add worker failure supervision and bounded restart attempts. [ ] Add explicit suspend/resume state transitions after the PSC identity gate is resolved.
3. Implement a Horizon network transport, enrollment/credential provisioning and authentication refresh. HTTP is the selected device transport. The encrypted-message layer must be integrated and replay-protected before public deployment; until then the existing plaintext endpoints must remain private.
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

The store uses fixed-size memory and performs no allocation, file I/O, database access, or network access. The HIPC status endpoint reports the store's active revision (zero until a future authenticated policy-delivery path publishes a snapshot). The model is not yet populated by the worker and does not enforce restrictions.

## Bounded policy JSON decoding

`protocol/policy_json.hpp` adds a fixed-buffer parser for schema v1. It accepts documents up to 16 KiB, uses a fixed token array, bounds nesting and string length, rejects unknown and duplicate root fields, requires every schema field, checks array lengths and integer ranges, and rejects malformed numbers and unsupported string escapes. No heap allocation is performed.

The accepted schema uses printable ASCII strings without escapes, integer-only numeric fields, numeric enum values, exactly seven weekday limits, and title-rule objects containing exactly `title_id` and `action`. This deliberately strict subset must match the server serializer. JSON decoding is followed by the existing policy validator through `DecodeAndValidate`; only a fully decoded and structurally valid candidate is returned. The decoder does not authenticate its input or publish to the active store.

## Deterministic policy evaluation (not enforcement)

`protocol/policy_evaluator.hpp` evaluates a supplied immutable policy and context without Horizon calls or side effects. It covers parent-authentication gating, offline behavior, explicit title blocks, allow-all versus allow-list-only title modes, fixed-offset weekday budgets and quiet hours including intervals that cross midnight. Explicit title allows do not override global quiet hours, daily budgets, parent authentication or offline restrictions.

The evaluator is host-tested. It is not yet called by the sysmodule worker and does not observe title launches, track playtime, mutate PCTL state, or block anything on a console. Monotonic accounting and a verified system integration point remain separate work.


## HTTP request construction (framing only)

`protocol/http_request.hpp` builds bounded HTTP/1.1 GET/POST requests into a
caller-owned buffer. It validates the method, DNS host, request path and bearer
token against header/path injection, emits a single `Host`, `Content-Length`,
`Connection: close` and `Accept-Encoding: identity`, and caps request bodies
at 16 KiB. The builder performs no allocation and does not own the output
buffer.

The request builder and response parser are tested independently on the host.
They are not a transport implementation: there is no DNS lookup, socket,
TLS handshake, certificate validation, redirect handling, retry policy or
credential provisioning yet.

## End-to-end encrypted HTTP payload protocol\n\nThe selected transport is HTTP with an application-layer authenticated-encryption envelope. `docs/secure-http-protocol.md` defines AES-128-GCM, HKDF-SHA-256 key domains, 96-bit nonces, 128-bit tags, and associated data binding each message to its direction, HTTP method, path, and device ID. The Python reference primitive is in `server/secure_transport.py` and is host-tested. It is not yet integrated into FastAPI routes or the Horizon sysmodule; therefore the current API still accepts plaintext and is **not safe to expose over public HTTP**. The remaining work includes encrypted one-time enrollment bootstrap, recoverable server-side device-key protection, Switch-side AES-GCM and HKDF, request sequencing/replay rejection, and cross-language known-answer tests.\n\n## HTTP and Cloudflare deployment decision

The selected device-facing transport is HTTP with end-to-end AES-128-GCM
payload encryption. The FastAPI backend now exposes
`POST /v1/secure/{device_id}/{operation}`; legacy plaintext device routes are
disabled by default. The envelope binds request/response direction, exact
path, device ID and a random request identifier as GCM associated data.
Access tokens are carried only inside ciphertext.

The backend derives the device key from the SHA-256 hash already stored for
the device credential. Enrollment uses the one-time invitation-code hash as
the bootstrap key. The Switch must hash the raw UTF-8 secret first, then use
HKDF-SHA-256 with salt `NX-PCTL-AEAD-1` and info
`NX-PCTL-AEAD/1/{purpose}/{device_id}`.

The new `source/secure_crypto.cpp` implements AES-128-GCM using the pinned
libnx AES-ECB hardware primitive plus a project GHASH implementation. It also
uses libnx SHA-256, HMAC-SHA-256 and OS-seeded random generation for HKDF,
nonces and request identifiers. Its self-test includes NIST SP 800-38D AES-GCM
known-answer vectors and a backend-matched HKDF vector. This source is built
with the sysmodule, but is not yet called by the worker or connected to the
HTTP client. It must pass CI compilation and a real-device self-test before
the network code may use it.

Cloudflare can proxy ciphertext but still sees IP addresses, hostname, URL
paths, headers, packet sizes and timing and can block or delay traffic. API
paths must bypass caching and body transformations; do not apply redirects,
Access login or browser challenges. Keep the origin private. The Python
backend routes are integrated, but public use remains blocked until the
Switch-side wire encoder/decoder, HTTP client, cross-language vectors and
full device workflow are tested.

## Network service permissions and implementation audit

The pinned Atmosphere source contains a Horizon socket backend in
`libraries/libstratosphere/source/socket/impl/socket_api.os.horizon.cpp`.
It initializes BSD through libnx using the system-client mode when
`socket::SystemConfigDefault` is selected. Its resolver entry points are
provided by libnx's `getaddrinfo` implementation and call the
`sfdnsres` service. The pinned libnx SSL implementation requests `ssl`
when using its default service type.

Based on those concrete call paths, the NPDM `service_access` list now
contains only:

- `bsd:s` — system BSD socket session used by the Atmosphere socket backend.
- `sfdnsres` — hostname resolution used by libnx `getaddrinfo`.
- `ssl` — libnx system SSL context/connection service.

NIFM is intentionally not requested: the audited socket initialization path
does not call `nifmInitialize`. The NPDM syscall list now also includes
`svcCreateTransferMemory` (`0x15`): the pinned BSD client calls
`tmemCreateFromMemory`, which invokes this syscall to share its configured
socket memory pool with `bsd:s`. The audited path does not call
`svcMapTransferMemory`/`svcUnmapTransferMemory`, so those capabilities were
not added. The permissions are not yet hardware-validated and must be checked
against the exact target firmware and service-manager policy before installation.

The production transport contract is HTTP POST to the configured public API
hostname, with all device-specific request and response bodies protected by
the application-layer AES-GCM envelope. DNS resolution uses the platform
resolver; sockets require bounded connect/read timeouts, bounded request and
response buffers, and connection closure after each request. The HTTP
framing/parser helpers and crypto primitives do not yet constitute a complete
network client. No credentials or policy are sent until the end-to-end
encryption, replay checks and response authentication are active.
