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

The NPDM title ID `0x010000000000D7A1` is a provisional development value,
not a registered or hardware-validated title ID. Do not install this package
on a Switch until the title ID, service permissions, kernel capabilities,
firmware compatibility and boot/recovery procedure have been reviewed.

The current experimental module initializes the service-manager client,
starts a resident worker and supervisor, and exposes the read-only `nxpctl`
HIPC diagnostics service. It also contains a fixed-size RAM-only policy
snapshot store and a bounded schema-v1 JSON decoder, but no code currently
loads a policy into the store. It has no network access, no PCTL mutation, no
filesystem permissions and no system-wide enforcement.

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

The backend supports plain HTTP for development and isolated trusted networks.
For traffic crossing an untrusted network, terminate TLS at a reverse proxy.
Plain HTTP exposes credentials and policy data to network observers and is not
an authenticated transport.

This does **not** mean the sysmodule is network-connected yet. The Switch-side
HTTP client, credential provisioning/storage, token refresh and worker
integration remain pending. The backend is server-side only at this stage.
