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

Build with a Bash-compatible shell. The commands below keep the libnx install
inside this repository's ignored `toolchain/` directory; they do not replace
the libnx installation under `/opt/devkitpro`:

```sh
SYSTEM_DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
PROJECT_TOOLCHAIN="$PWD/toolchain"
mkdir -p "$PROJECT_TOOLCHAIN"
ln -sfn "$SYSTEM_DEVKITPRO/devkitA64" "$PROJECT_TOOLCHAIN/devkitA64"
if [ -d "$SYSTEM_DEVKITPRO/portlibs" ]; then
  ln -sfn "$SYSTEM_DEVKITPRO/portlibs" "$PROJECT_TOOLCHAIN/portlibs"
fi
export DEVKITPRO="$PROJECT_TOOLCHAIN"
export DEVKITA64="$PROJECT_TOOLCHAIN/devkitA64"
export PATH="$DEVKITA64/bin:$SYSTEM_DEVKITPRO/tools/bin:$PATH"

make -C deps/libnx -j"$(nproc)"
make -C deps/libnx install
make -C sysmodule ATMOSPHERE_ROOT=/tmp/nxpctl-atmosphere nx_release
```

The pinned libnx install is written to `$DEVKITPRO/libnx`, which now resolves
to this project's `toolchain/libnx`. The existing devkitA64 compiler and
packaging tools are reused read-only through their original installation.

Before building the sysmodule, verify the two APIs from their respective
pinned source trees: `FsStorageForBatchRead` comes from libnx, while the
read-only `GetRunningApplicationProcessId` wrapper comes from Atmosphère's
libstratosphere headers.

```sh
grep -n "FsStorageForBatchRead" "$DEVKITPRO/libnx/include/switch/services/fs.h"
grep -n "GetRunningApplicationProcessId" \
  /tmp/nxpctl-atmosphere/libraries/libstratosphere/include/stratosphere/pgl/pgl_shell_api.hpp
```

The expected release package is:

```text
sysmodule/out/nintendo_nx_arm64_armv8a/release/nx_pctl_manager.nsp
```

Generated `build/`, `out/`, and `toolchain/` directories are local build
products and must not be committed.

## CI build

The `sysmodule-build` workflow:

1. runs the companion backend tests;
2. compiles and runs the shared C++ protocol/accounting tests;
3. initializes the pinned libnx submodule;
4. checks out Atmosphère and switch-tools at their pinned revisions;
5. builds host packaging utilities outside the devkitPro container;
6. creates a project-local toolchain directory in the container and installs
   the pinned libnx there;
7. builds, verifies and uploads the experimental NSP artifact.

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
