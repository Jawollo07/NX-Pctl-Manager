# NX-Pctl-Manager

NX-Pctl-Manager is an experimental, backend-managed, headless Atmosphère
sysmodule for **global application usage limits** on Nintendo Switch.

The historical project name is retained, but the implementation does not use
Nintendo's PCTL service or API. It does not implement per-title/game rules or
store title IDs. Management is performed through the companion FastAPI
backend and browser dashboard.

## Architecture

- `sysmodule/` — headless Atmosphère sysmodule, HIPC diagnostics and PGL
  application-process enforcement.
- `protocol/` — Horizon-independent policy, provisioning, secure transport
  and global usage contracts.
- `server/` — FastAPI/SQLite control plane and browser administration UI.
- `tests/` — host-side C++ protocol and accounting tests.
- `docs/` — security, protocol, build and deployment documentation.
- `deps/libnx` — pinned libnx submodule.

The backend owns device enrollment, global policy revisions, TOTP parent
authentication, grants and revocation. Device messages use authenticated
AES-GCM-encrypted bodies over HTTP; HTTP metadata remains visible.

## Current status

The feature branch builds an experimental NSP in GitHub Actions and runs the
backend and host-side protocol tests. This is not a production release.
First-time enrollment still needs a separate trusted HIPC provisioning client.
Suspend/resume accounting, firmware-specific compatibility review, boot
recovery and physical-console enforcement tests remain outstanding.

PGL enforcement terminates application processes when the global policy
denies use. It does not lock HOME/qlaunch, system settings, recovery mode or
the boot chain. A person who controls the console, SD card or CFW can bypass
the module. Do not describe this as an unbypassable whole-console lock.

## Build and deployment

- [Sysmodule build guide](docs/sysmodule-build.md)
- [Backend deployment guide](docs/deployment.md)
- [HTTP protocol](docs/secure-http-protocol.md)
- [Policy schema](docs/policy-schema-v1.md)
- [Security model](docs/security-model.md)
- [Implementation and release gates](docs/sysmodule-migration-plan.md)

Do not install the experimental NSP until the NPDM title ID, service
permissions, firmware compatibility, persistence and recovery path have been
independently reviewed and tested on real hardware.
