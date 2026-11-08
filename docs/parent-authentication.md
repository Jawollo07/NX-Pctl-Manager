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
