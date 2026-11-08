# Backend-managed parent authentication

Parent authentication is owned by the backend. The Switch never receives the
TOTP seed and has no local setup, rotation, recovery, or administrative HIPC
command for it.

## Backend implementation

- RFC 6238 TOTP: HMAC-SHA1, six digits, 30-second step, ±1 step tolerance.
- A successful verification creates a 15-minute parent-auth grant.
- Each accepted TOTP counter can be used only once.
- Five failed attempts trigger a five-minute lockout.
- TOTP secrets are encrypted at rest with AES-256-GCM. The device ID is bound
  as associated data.
- The plaintext seed is displayed once during provisioning. Status endpoints
  never return it.
- The Switch reads only `parent_authenticated` and `authenticated_until`
  through the authenticated encrypted device operation
  `POST /v1/secure/{device_id}/parent-auth`.

The admin API is protected by `NXPCTL_ADMIN_TOKEN`. Secret encryption requires
a separate 32-byte key in `NXPCTL_PARENT_AUTH_KEY` (64 hexadecimal
characters). There is no insecure fallback if either value is missing.

## Administration

Provision/rotate a seed:

```http
POST /v1/admin/devices/{device_id}/parent-auth
Authorization: Bearer <NXPCTL_ADMIN_TOKEN>
Content-Type: application/json

{"confirm":true}
```

Verify a current code and issue the grant:

```http
POST /v1/admin/devices/{device_id}/parent-auth/verify
Authorization: Bearer <NXPCTL_ADMIN_TOKEN>
Content-Type: application/json

{"code":"123456"}
```

Read status with `GET /v1/admin/devices/{device_id}/parent-auth`; revoke the
configuration with `DELETE /v1/admin/devices/{device_id}/parent-auth`.

## Integration status and limits

The backend API, encrypted secret storage, replay protection, lockout, and
secure device grant response are implemented and covered by host-side tests.
The Switch synchronization worker now polls the grant through the encrypted
`parent-auth` operation and supplies the result to the global policy evaluator.
When the active policy requires parent authentication, global application
execution is denied unless the grant is valid according to the trusted backend
clock. A previously received grant remains valid offline only until its
15-minute expiry; after reboot, the volatile grant must be fetched again.
Hardware validation is still outstanding.

This is application-level authentication only. It does not replace Nintendo's
native parental-control PIN or authenticate Nintendo system screens.