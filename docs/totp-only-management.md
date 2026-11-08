# Backend-only parent-authentication management

Parent-authentication setup and control are performed by the backend API. No
TOTP secret is stored on the Switch SD card, and the Switch exposes no local
secret provisioning or rotation command.

The backend generates the TOTP secret, displays it once to the administrator,
and stores only an AES-256-GCM encrypted copy. The administrator verifies
six-digit codes through the authenticated backend API. A successful check
creates a 15-minute authorization grant; replayed codes are rejected and
five failed attempts cause a five-minute lockout.

The Switch can only retrieve the grant state through its authenticated,
encrypted device channel. Runtime polling and policy enforcement integration
are still pending, so this is not yet an active end-to-end console lock.

This feature is independent of Nintendo's native parental-control PIN and
does not change the authentication behavior of Horizon system screens.