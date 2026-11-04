# NX-Pctl-Manager — Sysmodule

NX-Pctl-Manager is being developed as a headless Atmosphère/Stratosphère system
module for Nintendo Switch. The former standalone homebrew application and its
Borealis UI have been removed from this branch. The repository now focuses on
the resident sysmodule, its shared protocol, tests, and the companion backend.

## Repository layout

- `sysmodule/` — Stratosphere system module, NPDM manifest and build rules.
- `protocol/` — Horizon-independent policy, provisioning, HTTP and usage
  accounting contracts shared with the module and backend.
- `tests/` — host-side C++ tests for protocol, policy and accounting logic.
- `server/` — companion HTTP backend used for encrypted synchronization.
- `docs/` — architecture, security model, protocol and build documentation.
- `deps/libnx` — pinned libnx submodule required by the build.

There is no NRO target, Borealis dependency, on-console UI, or UI resource tree
in this branch. Management and provisioning are intended to happen through
the companion backend and the sysmodule's authenticated protocol.

## Build

The build is experimental and requires devkitPro/devkitA64, GNU Make, Git, the
pinned libnx submodule, a pinned Atmosphère source checkout, and the host
utilities `npdmtool`, `elf2nso` and `build_pfs0`.

See [the reproducible sysmodule build guide](docs/sysmodule-build.md) for exact
source revisions and commands.

## Current implementation status

The module currently provides encrypted HTTP synchronization, local IPC,
credential storage, and read-only global application activity monitoring
through PGL. Global daily playtime is persisted in alternating device-key-
derived AES-GCM records in the sysmodule's own save data. Interrupted writes
preserve the newest valid slot; a trusted clock regression cannot erase a
newer saved day. This is not policy enforcement. Hardware behavior,
suspend/resume accounting, recovery and enforcement remain unfinished and
must not be assumed safe for installation.

The CI workflow runs backend tests, shared C++ protocol/accounting tests, and
an experimental Stratosphere package build. A successful CI build is not
equivalent to hardware validation.

## Safety

Do not install an experimental NSP on a console until the NPDM title ID,
service permissions, kernel capabilities, firmware compatibility, persistent
state handling, and a tested boot-recovery procedure have been independently
reviewed. No real-hardware validation is claimed by this repository.
