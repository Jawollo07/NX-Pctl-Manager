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

Build with a Bash-compatible shell. The helper verifies both pinned
revisions, initializes the libnx submodule, installs libnx into this
repository's ignored `toolchain/` directory, checks the required APIs, and
builds the release package. It reuses the installed devkitA64 compiler and
packaging utilities without changing the system libnx installation.

```sh
export ATMOSPHERE_ROOT=/tmp/nxpctl-atmosphere
bash scripts/build-sysmodule-local.sh
```

The script expects the Atmosphère checkout to be at revision
`6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8` and libnx at
`b156b4774fb2df7b93402169509266da7547fc4c`. If devkitPro is installed in a
non-standard location, set `DEVKITPRO` to that existing installation before
running the script. All libnx install output stays under this repository's
`toolchain/libnx`; `/opt/devkitpro` is not modified.

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

The current module includes encrypted network synchronization, local IPC,
protected credential storage, global daily activity monitoring, and PCTL-free
global enforcement through the pinned PGL `TerminateProcess` API. The runtime
uses only the current application PID transiently; it does not retain title IDs
or create per-title usage/rules. A denied global policy terminates the running
application. An enrolled device without a usable verified policy/trusted clock
fails closed. This is process termination, not a whole-console lock; the
one-second monitor leaves a short relaunch window and hardware validation is
still required. Global daily usage is stored in alternating AES-128-GCM-
authenticated snapshots in the sysmodule's own device save data. The newest
valid sequence is restored after restart; a trusted clock regression cannot
roll the saved day backwards. Periodic writes are flushed once per minute
while dirty and on orderly shutdown. A sudden power loss may lose up to about
one minute of new usage.

Do not install the NSP on hardware until the NPDM title ID, service permissions,
kernel capabilities, firmware compatibility and a boot-recovery procedure
have been independently reviewed and tested.
