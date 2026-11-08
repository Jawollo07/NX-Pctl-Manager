# NX-Pctl-Manager Sysmodule (experimental)

This directory contains the headless Atmosphère/Stratosphère sysmodule. The
legacy NRO/Borealis application is not part of this feature branch.

## Pinned build dependencies

- Atmosphère: `6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8`
- libnx: `b156b4774fb2df7b93402169509266da7547fc4c` (`deps/libnx`)
- switch-tools: `22756068dd0ed6ff9734c59cb4f99ebd3f62555b` (CI host tools)

Atmosphère is an external checkout, not a Git submodule. Build only against
the documented pinned revision.

## Local build

Install devkitPro/devkitA64 and the pinned host tools, then:

```sh
git submodule update --init deps/libnx
git clone https://github.com/Atmosphere-NX/Atmosphere.git /tmp/nxpctl-atmosphere
git -C /tmp/nxpctl-atmosphere checkout 6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8
export ATMOSPHERE_ROOT=/tmp/nxpctl-atmosphere
bash scripts/build-sysmodule-local.sh
```

The script verifies both pinned revisions and required APIs. It does not pull,
rebase, reset or otherwise update the project checkout. Full details:
[reproducible build guide](../docs/sysmodule-build.md).

Expected artifact:
`sysmodule/out/nintendo_nx_arm64_armv8a/release/nx_pctl_manager.nsp`.

## Implemented runtime

- Headless resident sysmodule with a narrow `nxpctl` HIPC service.
- Encrypted HTTP device protocol using AES-128-GCM/HKDF, random request IDs,
  response authentication and backend replay protection.
- Backend enrollment, authentication, short-lived access tokens, heartbeat,
  policy fetch/acknowledgement and parent-auth grant polling.
- Global application-active time accounting, fixed-offset day rollover and
  encrypted alternating usage snapshots in the module's own save data.
- Backend-managed global daily/weekday limits, quiet hours and offline rules.
- Backend-managed TOTP parent authentication. The TOTP seed remains encrypted
  on the server and never reaches the Switch.
- PGL process-start observation and one-second accounting/enforcement
  fallback. Only the current application PID is used transiently for
  `pgl::TerminateProcess`; no title IDs or per-title records are retained.
- HIPC diagnostics for runtime, usage, policy decision, enforcement and
  authenticated connectivity.

No Nintendo PCTL service/API is used. There are no per-game restrictions,
per-title histories, or on-console administrator screens.

## Important limitations

This enforcement boundary terminates application processes. It does not lock
HOME/qlaunch, system settings, recovery mode or the boot chain, and it is not
tamper-proof against someone who controls the console, SD card or CFW.
Suspend/resume accounting is not implemented because a safe PSC module
identity has not been established.

The branch includes `tools/provisioner/nxpctl-bootstrap.nro`, a one-shot,
non-interactive HIPC bootstrap helper. It reads the human-readable
`nxpctl.conf` file from the SD-card root, strictly validates its three required
keys, submits the fixed-size request through command 65001 and deletes the file
after the sysmodule accepts the request. The host-side Python generator prompts
for the API hostname, device ID and one-time enrollment code without echoing
the code. See [device enrollment](../docs/device-enrollment.md). This is not a
management UI and does not use Borealis.

The configured NPDM title ID is a development value and has not been
independently reviewed or validated on hardware. CI packaging does not prove
runtime safety. Do not install the experimental NSP until title ID, service
permissions, capabilities, firmware compatibility, save-data behavior and a
tested recovery/uninstall procedure have been verified on a physical console.

See [runtime details](RUNTIME.md), [target architecture](../docs/full-parental-control-architecture.md)
and the [implementation gates](../docs/sysmodule-migration-plan.md).
