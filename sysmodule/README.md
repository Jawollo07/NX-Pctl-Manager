# NX-Pctl-Manager Sysmodule (experimental)

This directory is the first independent, headless Stratosphere sysmodule target.
It does not replace the legacy Borealis NRO build.

## Pinned build dependency

The initial build is tied to Atmosphere commit:

`6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8`

Use a complete source checkout at that exact revision. Do not build against
Atmosphere `master` or an arbitrary release and assume compatibility.

```sh
git clone https://github.com/Atmosphere-NX/Atmosphere.git
git -C Atmosphere checkout 6b0c077b4c608a3a2ee712b8dc35fee87ceb62e8
make -C sysmodule ATMOSPHERE_ROOT="$PWD/Atmosphere" nx_release
```

The output is written under the Atmosphere-derived `sysmodule/out/` directory.
The expected first artifact is `nx_pctl_manager.nsp`.

## Development-only metadata

The NPDM title ID `0x010000000000D7A1` is a provisional development value,
not a registered or hardware-validated title ID. Do not install this package
on a Switch until the title ID, service permissions, kernel capabilities,
firmware compatibility and boot/recovery procedure have been reviewed.

The current module only initializes the service-manager client and remains
resident in an idle loop. It has no network access, no local IPC service, no
PCTL mutation, no filesystem permissions and no system-wide enforcement.

## Build requirements

- devkitA64 and libnx from devkitPro;
- the Atmosphere source tree at the pinned revision;
- Atmosphere's libstratosphere sources and build tools, including `npdmtool`;
- GNU Make and the compiler versions supported by that Atmosphere revision.

A successful host-side compile is not evidence that the module is safe to load.
Hardware installation/testing is intentionally not part of this phase.
