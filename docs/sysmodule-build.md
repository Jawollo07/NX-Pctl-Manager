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

The libnx revision is stored as the `deps/libnx` Git submodule. Atmosphere is
also pinned as a submodule in this branch. Do not silently update these
revisions: the compiler-facing APIs must be validated together.

## Local build

Prerequisites:

- devkitPro devkitA64 toolchain;
- GNU Make and Git;
- `switch-tools` host utilities (`npdmtool`, `elf2nso`, `build_pfs0`);
- all repository submodules initialized.

From the repository root, in a Bash-compatible shell:

```sh
git submodule update --init --recursive

export DEVKITPRO=/opt/devkitpro
export PATH="$DEVKITPRO/devkitA64/bin:$DEVKITPRO/tools/bin:$PATH"

make -C deps/libnx -j"$(nproc)"
make -C deps/libnx install

make -C sysmodule ATMOSPHERE_ROOT="$PWD/Atmosphere" nx_release
```

If your devkitPro installation is in another directory, change
`DEVKITPRO` accordingly. The pinned libnx install writes to
`$DEVKITPRO/libnx`; use a dedicated development toolchain if you do not want
to replace the system's currently installed libnx.

The expected release package is:

```text
sysmodule/out/nintendo_nx_arm64_armv8a/release/nx_pctl_manager.nsp
```

The generated `build/` and `out/` directories are local build products and
must not be committed.

## CI build

The `sysmodule-build` GitHub Actions workflow:

1. checks out the project and initializes only the pinned libnx submodule;
2. checks out Atmosphere at its pinned commit;
3. builds the host-side `switch-tools` utilities from their pinned source;
4. builds and installs the pinned libnx into the ephemeral devkitPro container;
5. builds the sysmodule and verifies that the expected NSP is non-empty;
6. uploads the NSP as an experimental CI artifact.

The host-side packaging tools are built outside the devkitPro container because
the devkitPro package endpoint may be unavailable to GitHub-hosted runners.
They are mounted into the container as host executables; both environments
run on the same x86_64 runner.

## Scope and safety

The current module includes encrypted network synchronization, local IPC, and
read-only global application-activity monitoring through PGL for daily playtime.
It does not yet enforce policies, write PCTL state, persist daily usage across
restarts, or implement the hardware recovery path.

The NPDM title ID in `nx_pctl_manager.json` is a provisional development
placeholder. Do not install the NSP on hardware until the title ID, NPDM
permissions, kernel capabilities, firmware compatibility and a boot-recovery
procedure have been independently reviewed and tested.
