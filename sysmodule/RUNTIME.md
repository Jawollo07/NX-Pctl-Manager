# Sysmodule runtime

## Current runtime phase

The experimental Stratosphere target now has a resident main thread, a separate cooperative worker thread, and a read-only local HIPC service.

- `InitializeSystemModule` initializes the service-manager client, checks the pinned libstratosphere API, and creates the worker stop event.
- `Main` starts the worker, registers the `nxpctl` HIPC service, and enters the Stratosphere server loop.
- The worker uses a one-second timed event wait as its idle cadence and cooperative shutdown point.
- HIPC command 65000 (`GetRuntimeStatus`) returns protocol version, worker state, worker heartbeat, and policy revision. The initial policy revision is zero.
- `FinalizeSystemModule` signals the worker, joins it, destroys its thread object, and finalizes the event.

The HIPC interface is read-only. It does not inspect running titles, change parental-control state, apply restrictions, access persistent storage, or perform network I/O. No policy is persisted on the console.

## Next implementation phases

1. Add client-side IPC tests and verify service access from an explicitly authorized companion process.
2. Add lifecycle supervision and explicit suspend/resume state transitions.
3. Add authenticated HTTPS/WSS transport to the management server.
4. Add policy validation and in-memory policy snapshots.
5. Implement enforcement only after the required Horizon services, permissions, firmware compatibility, and recovery behavior have been validated.

## Safety boundary

The NPDM title ID remains provisional. Do not install or launch this artifact on a console until the title ID, service permissions, kernel capabilities, firmware compatibility, and recovery procedure have been reviewed. CI packaging does not constitute hardware validation.
