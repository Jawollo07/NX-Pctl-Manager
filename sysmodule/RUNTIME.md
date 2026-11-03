# Sysmodule runtime

## Current runtime phase

The experimental Stratosphere target now has a resident main thread, a separate cooperative worker thread, and a read-only local HIPC service.

- `InitializeSystemModule` initializes the service-manager client, checks the pinned libstratosphere API, and creates the worker stop event.
- `Main` starts the worker, registers the `nxpctl` HIPC service, and enters the Stratosphere server loop.
- The worker uses a one-second timed event wait as its idle cadence and cooperative shutdown point. A separate supervisor checks thread termination with `os::TryWaitThread`, reaps the exited thread, and permits at most three automatic restarts per module lifetime. After the retry budget is exhausted, the service reports `WorkerState::Failed` and does not restart indefinitely.
- Finalization stops and joins the supervisor before stopping the worker, preventing an intentional shutdown from being mistaken for a worker failure.
- HIPC command 65000 (`GetRuntimeStatus`) returns protocol version, worker state, worker heartbeat, bounded worker restart count, and policy revision. The initial policy revision is zero.
- `protocol/runtime_status.hpp` is the shared, Horizon-independent wire contract used by both the NRO client and sysmodule. `protocol/worker_supervision.hpp` contains the Horizon-independent three-restart budget used by the supervisor; CI tests that budget independently. `WorkerState::Failed` is a terminal health state after the bounded restart budget is exhausted. The CMIF output layout is four `u32` values at offsets 0/4/8/12 (protocol version, worker state, heartbeat, restart count), then the aligned `u64` policy revision at offset 16.
- `protocol/http_response.hpp` now provides a bounded, allocation-free HTTP/1.0/1.1 response parser. It requires exactly one decimal `Content-Length`, rejects chunked/other transfer encodings, rejects compressed bodies, duplicate lengths, malformed CRLF framing, truncated bodies and trailing bytes, and caps headers/body at 8 KiB/16 KiB. It returns a view into the caller-owned receive buffer; it does not perform socket I/O or authenticate the peer.
- The existing NRO performs an optional read-only status request at startup. If the service is absent, it logs the condition and continues in standalone mode.
- `FinalizeSystemModule` signals the worker, joins it, destroys its thread object, and finalizes the event.

The HIPC interface is read-only. It does not inspect running titles, change parental-control state, apply restrictions, access persistent storage, or perform network I/O. No policy is persisted on the console.

## Next implementation phases

1. Verify live HIPC session establishment and command dispatch on supported hardware/CFW; the current CI test validates the shared ABI, not the Horizon IPC transport.
2. [x] Add worker failure supervision and bounded restart attempts. [ ] Add explicit suspend/resume state transitions after the PSC identity gate is resolved.
3. Implement a Horizon network transport, enrollment/credential provisioning and authentication refresh. Plain HTTP is supported by the backend for trusted isolated LAN use, but it does not provide confidentiality or integrity; never treat policy data as trusted on plaintext transport without a separate authenticated integrity mechanism. HTTPS server-identity validation is required on untrusted networks.
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

## Cloudflare deployment requirement

The intended public deployment is Switch → HTTPS on Cloudflare-proxied
hostname (port 443) → Cloudflare → HTTPS origin in Full (strict) mode →
reverse proxy → loopback-only backend HTTP listener. The device must validate
the Cloudflare edge's public certificate chain and requested hostname. A
Cloudflare Origin CA certificate is for the Cloudflare-to-origin connection;
it is not a trust anchor for the Switch's edge connection. API paths must
bypass caching and interactive Cloudflare Access/challenge pages.

Plain HTTP remains useful only for isolated development. It is not a
production fallback when the hostname is proxied. Cloudflare terminates TLS,
so TLS alone does not provide cryptographic integrity against Cloudflare
itself; signed policy snapshots with a pinned public verification key remain
a future hardening step.


## Pinned libnx TLS API audit

The pinned libnx submodule revision `b156b4774fb2df7b93402169509266da7547fc4c`
contains the Switch SSL service client API (`sslInitialize`, SSL context and
connection objects, hostname setup, socket descriptor binding, handshake,
read/write, and certificate verification error reporting). The API documents
the default peer-CA and hostname verification flags and use of the system
certificate store. This is the intended TLS implementation path for the
Cloudflare-proxied hostname; a custom TLS stack or a Cloudflare Origin CA
trust anchor is not planned.

The next transport implementation must explicitly retain peer-CA, hostname
and certificate-date verification, set the TLS SNI/hostname to the configured
public API hostname, and fail closed on any certificate-verification error.
It must not use `SslOptionType_SkipDefaultVerify` or disable verification.
The socket must connect to the DNS-resolved Cloudflare address while the TLS
connection and HTTP `Host` header use the configured hostname.

The service ACL/NPDM requirements for the sysmodule's NIFM, BSD socket and SSL
service access have not yet been validated against the pinned Atmosphere
runtime. Do not add guessed service ACL entries or start network access until
those requirements and the target firmware compatibility are verified.

## Network service API audit (pinned libnx)

The pinned libnx revision was inspected directly at
`b156b4774fb2df7b93402169509266da7547fc4c`:

- `nifmInitialize(NifmServiceType_User)` requests `nifm:u`.
- BSD initialization selects `bsd:s` when the system-service flag is supplied;
  it can fall back to `bsd:u` only when the user-service flag is also supplied.
  The project must choose the least-privileged mode supported by the target
  firmware and must not assume either mode is available to a sysmodule.
- SSL's default service selection requests `ssl`. The explicit system service
  selection requests `ssl:s` on Horizon 15.0.0 and newer; older firmware falls
  back to the default `ssl` service.
- libnx documents the default SSL verification bitmask as peer-CA plus hostname.
  Date validation is a separate `SslVerifyOption_DateCheck` bit and must be
  explicitly included by the client. The API requires peer-CA and hostname
  verification to remain enabled; do not use `SslOptionType_SkipDefaultVerify`.

These are the service names selected by the pinned client library, not proof
that the proposed sysmodule is authorized to access them. The NPDM
`service_access` entries, service-manager policy, socket initialization
requirements, and firmware-specific behavior still need a source-level and
hardware-compatible validation. Therefore `service_access` remains empty and
network code remains disabled; no ACL or kernel capability is inferred from
the names above.
