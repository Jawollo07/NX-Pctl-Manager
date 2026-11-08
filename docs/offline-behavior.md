# Offline behavior and connection freshness

The sysmodule does not infer connectivity from a configured hostname, a DNS
result, a TCP socket, or an unauthenticated HTTP response. Connectivity becomes
online only after the secure HTTP client receives and authenticates a complete
AES-GCM response from the configured backend.

The last authenticated response is timestamped using the Horizon monotonic
system tick. The device is considered online for 120 seconds after that
response. After the grace period it is offline. Before the first authenticated
response after boot, connectivity is reported as unknown and policy evaluation
treats unknown as offline. A monotonic regression is also treated conservatively
as unknown/offline.

Policy behavior while offline is explicit:

- `KeepLastVerified`: continue evaluating the last in-memory, authenticated
  policy and its trusted server-time anchor until the policy expires. The
  backend is not required for each one-second enforcement decision.
- `BlockAllUsage`: deny application use as soon as connectivity is unknown or
  the 120-second authenticated-contact grace period expires.
- An enrolled device without a valid policy or trusted clock remains
  fail-closed independently of the offline setting.

Only authenticated server responses refresh the connection timer. Failed
requests do not extend the grace period. A successful authenticated response
to enrollment, authentication, heartbeat, policy retrieval, or acknowledgement
does refresh it.

HIPC command 65005 (`GetConnectivityStatus`) returns:
- state 0: unknown (no authenticated response since boot, or monotonic anomaly);
- state 1: online (authenticated response no more than 120 seconds ago);
- state 2: offline (authenticated response is older than 120 seconds);
- last-success age in seconds, or `UINT64_MAX` when unknown.

This state is volatile and is reset on sysmodule restart. It does not persist
credentials, tokens, policies, or connection timestamps.
