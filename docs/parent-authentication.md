# Local parent-authentication foundation

This branch now includes a platform-independent RFC 6238 TOTP verifier in
`protocol/totp.hpp`. It uses HMAC-SHA1, six decimal digits, a 30-second time
step, and a configurable time-step window (default: one step on either side).
The verifier rejects malformed input and counters at or below the caller's
last accepted counter, allowing the integration layer to enforce replay
protection.

The verifier deliberately does not own or persist the shared secret, trusted
clock, last accepted counter, lockout state, or authenticated-session expiry.
Those values must be provided by the sysmodule's encrypted save-data layer.
The next integration stage must add an authenticated provisioning path for the
secret, durable anti-replay state, bounded failed-attempt lockout, and a
short-lived HIPC parent-authentication session. Until that work is complete,
this is a tested cryptographic primitive, not an active parental-authentication
feature. It does not replace Nintendo's native parental-controls PIN or
authenticate Nintendo system screens.

## Volatile authentication state machine

`protocol/parent_auth.hpp` adds a host-testable state machine around the TOTP
primitive. A successful code opens a 15-minute in-memory session. Five
consecutive invalid attempts trigger a five-minute monotonic-time lockout.
The same 30-second TOTP counter cannot be accepted twice by one configured
instance. Ending a session clears its authorization immediately.

The state machine is intentionally volatile. It does not yet persist the
shared secret, accepted counter, failed-attempt count, or lockout deadline, and
it is not yet exposed through HIPC or connected to the policy evaluator. The
platform integration must persist replay/lockout state in encrypted device
save data and must only install or rotate a secret through an authenticated
provisioning path before this can be treated as an active parent-auth feature.
