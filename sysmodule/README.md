# NX-Pctl-Manager Sysmodule (experimental)

This directory is the first independent, headless Stratosphere sysmodule target.
It does not replace the legacy Borealis NRO build.

## Pinned build dependencies

The initial build is tied to these exact source revisions:

- Atmosphere: `6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8`
- libnx: `b156b4774fb2df7b93402169509266da7547fc4c` (the `deps/libnx` submodule)
- switch-tools: `22756068dd0ed6ff9734c59cb4f99ebd3f62555b` (CI host tools)

Atmosphere is pinned as the `Atmosphere` submodule. Do not build against
Atmosphere `master` or an arbitrary release and assume compatibility.

## Local build

Initialize the submodules and build against the pinned libnx:

```sh
git submodule update --init --recursive

export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"

make -C deps/libnx -j"$(nproc)"
make -C deps/libnx install

make -C sysmodule ATMOSPHERE_ROOT="$PWD/Atmosphere" nx_release
```

Install the `switch-tools` host utilities separately if they are not already
available in your PATH. The full reproducible build instructions are in
[`docs/sysmodule-build.md`](../docs/sysmodule-build.md).

The output is written under the Atmosphere-derived `sysmodule/out/` directory.
The expected release package is
`sysmodule/out/nintendo_nx_arm64_armv8a/release/nx_pctl_manager.nsp`.

## Development-only metadata

The NPDM title ID `0x420000000A3F1E7C` is the currently configured development value,
not a registered or hardware-validated title ID. Do not install this package
on a Switch until the title ID, service permissions, kernel capabilities,
firmware compatibility and boot/recovery procedure have been reviewed.

The current experimental module initializes the service-manager client,
starts a resident worker and supervisor, and exposes the `nxpctl` HIPC
service. HIPC command 65000 reports runtime status; command 65001 accepts a
validated, fixed-size API-host/device-ID/one-time-code provisioning request.
The NRO client exposes the matching enrollment configuration call.

The worker performs encrypted enrollment, device authentication, short-lived
token refresh, heartbeats, policy retrieval, schema/clock/device/revision
validation, RAM-only policy publication and policy acknowledgements. The
HTTP-only Horizon BSD transport and AES-128-GCM/HKDF envelope client verify
response authenticity before plaintext reaches the policy decoder. The
provisioning lock is held only for bounded memory copies, never across network
I/O, so local HIPC requests are not blocked by DNS/TCP/HTTP waits.

The sysmodule stores the API host, device ID and long-lived device credential
in its own device save data. Records are AES-128-GCM encrypted with a key
derived through SPL from an application-owned key source and the console
device key ladder. Two alternating records provide recovery from interrupted
writes. The short-lived access token and one-time enrollment code remain
RAM-only; after restart, the sysmodule restores the device credential and
authenticates again for a fresh access token. The NPDM requests the device-save
data and own-save-data capabilities plus `fsp-srv` and `spl:crypto`. There is
still no PCTL mutation, title monitoring or system-wide enforcement. Hardware
validation has not been performed.

A successful host-side compile is not evidence that the module is safe to load.
Hardware installation/testing is intentionally not part of this phase.

The exact schema-v1 JSON profile is documented in [`docs/policy-schema-v1.md`](../docs/policy-schema-v1.md).

## Policy processing status

The current branch includes a bounded schema-v1 decoder and a pure policy evaluator with allow-all and allow-list-only title modes. Both are host-tested foundations only: no authenticated network path feeds them, and the evaluator is not connected to title monitoring or enforcement.


## HTTP backend

A reference FastAPI/SQLite backend now lives in [`server/`](../server/README.md).
It implements one-time enrollment, per-device credentials, short-lived bearer
tokens, strict policy retrieval, heartbeats, deduplicated event batches and
policy acknowledgements. Administration is performed through
`python -m server.cli`; there is intentionally no unauthenticated web-admin
surface.

The device API uses HTTP with application-layer AES-128-GCM encryption and
authentication for every request/response body. IP addresses, hostname, URL
path, headers, packet sizes and timing remain visible. The secure routes are
required; legacy plaintext routes are disabled by default.

The sysmodule now contains the TCP/HTTP transport and encrypted wire client.
It is not activated from the worker: trusted provisioning of the API host and
one-time enrollment code, followed by secure credential lifecycle handling,
must be implemented before the console can enroll or synchronize.
