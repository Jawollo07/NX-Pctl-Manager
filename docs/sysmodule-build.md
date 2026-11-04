# Reproducible sysmodule build

This document describes the experimental Stratosphere build used by
`feature/headless-sysmodule-migration`. It does not describe installation or
hardware validation.

## Pinned source revisions

| Component | Repository | Revision |
| --- | --- | --- |
| Atmosphere / libstratosphere | `Atmosphere-NX/Atmosphere` | `6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8` |
| libnx | `switchbrew/libnx` | `b156b4774fb2df7b93402169509266da7547fc4c` |
| switch-tools | `switchbrew/switch-tools` | `22756068dd0ed6ff9734c59cb4f99ebd3f62555b` |

Only libnx is kept as a repository submodule. Atmosphere and switch-tools are
checked out separately at the exact revisions above so this project does not
vendor or track their moving branches.

## Local build

Prerequisites:

- devkitPro devkitA64 toolchain;
- GNU Make and Git;
- `switch-tools` host utilities (`npdmtool`, `elf2nso`, `build_pfs0`);
- the pinned libnx submodule;
- an Atmosphère source checkout at the pinned revision.

Initialize libnx and obtain the external build dependencies:

```sh
git submodule update --init deps/libnx
git clone https://github.com/Atmosphere-NX/Atmosphere.git /tmp/nxpctl-atmosphere
git -C /tmp/nxpctl-atmosphere checkout 6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8
```

Build with a Bash-compatible shell:

```sh
export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"

make -C deps/libnx -j"$(nproc)"
make -C deps/libnx install
make -C sysmodule ATMOSPHERE_ROOT=/tmp/nxpctl-atmosphere nx_release
```

If devkitPro is installed elsewhere, adjust `DEVKITPRO`. The pinned libnx
install writes to `$DEVKITPRO/libnx`; use a dedicated development toolchain
if you do not want to replace the system's currently installed libnx.

The expected release package is:

```text
sysmodule/out/nintendo_nx_arm64_armv8a/release/nx_pctl_manager.nsp
```

Generated `build/` and `out/` directories are local build products and must
not be committed.

## CI build

The `sysmodule-build` workflow:

1. runs the companion backend tests;
2. compiles and runs the shared C++ protocol/accounting tests;
3. initializes the pinned libnx submodule;
4. checks out Atmosphère and switch-tools at their pinned revisions;
5. builds the host packaging utilities and the Stratosphere module;
6. verifies and uploads the experimental NSP artifact.

The packaging utilities are built outside the devkitPro container because
the devkitPro package endpoint may be unavailable to GitHub-hosted runners.
They are mounted into the container as host executables on the same x86_64
runner.

## Scope and safety

The current module includes encrypted network synchronization, local IPC, and
read-only global application-activity monitoring through PGL for daily playtime.
It does not yet enforce policies, write PCTL state, persist daily usage across
restarts, or implement the hardware recovery path.

Do not install the NSP on hardware until the NPDM title ID, service permissions,
kernel capabilities, firmware compatibility and a boot-recovery procedure
have been independently reviewed and tested.
