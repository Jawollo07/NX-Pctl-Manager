# End-to-end HTTP payload encryption (protocol v1)

## Security contract

The API transport may use HTTP, but **every device-specific request and response
body must be an authenticated-encryption envelope** before it is sent over the
network. The cryptographic boundary is Switch ↔ backend; Cloudflare is not
given a decryption key. AES-128-GCM provides confidentiality and integrity.
This does not hide IP addresses, hostnames, URL paths, HTTP headers, packet
sizes or timing, and it cannot prevent blocking or traffic analysis.

The first implementation increment adds the Python cryptographic primitive
and host tests. It is not yet wired into the FastAPI routes or the Horizon
worker. Until those integrations and matching cross-language test vectors
land, the existing endpoints remain plaintext and must not be exposed on an
untrusted network.

## Envelope

The JSON wire envelope is exactly:

```json
{"v":1,"n":"BASE64URL_NONCE","c":"BASE64URL_CIPHERTEXT_AND_TAG"}
```

- AES-128-GCM, 16-byte key, 12-byte nonce, 16-byte authentication tag.
- Base64url without padding; no additional envelope fields.
- Maximum plaintext: 64 KiB.
- The GCM associated data is the ASCII string
  `NX-PCTL-AEAD/1\n{direction}\n{method}\n{path}\n{device_id}`.
- Direction is `request` or `response`; method is currently GET or POST.
  Thus a valid ciphertext cannot be moved to another endpoint, device, HTTP
  method or direction.
- A fresh cryptographically random nonce is mandatory for every encryption
  under a given key. Nonce reuse with AES-GCM is catastrophic.

## Key derivation and bootstrap

HKDF-SHA-256 derives a 128-bit AES key from secret material, using salt
`NX-PCTL-AEAD-1` and info
`NX-PCTL-AEAD/1/{purpose}/{device_id}`. The purpose is either
`enrollment` or `device`, preventing cross-protocol key reuse.

Enrollment must not send the invitation code in cleartext. The Switch derives
the enrollment key from the one-time invitation code and device ID, then sends
an encrypted enrollment envelope. The backend validates/decrypts it and
returns the newly generated device credential inside a response envelope
under the same bootstrap key. Both sides then derive the long-term device key
from the device credential. Authentication, token exchange, policy delivery,
heartbeats, event batches and acknowledgements must use that device key.

The backend must retain a recoverable copy of the device credential for
decryption, protected at rest by a separately provisioned server master key;
a one-way hash alone is insufficient for AEAD decryption. The master key must
come from a secret manager or protected environment injection, never from the
repository or SQLite plaintext. Re-enrollment rotates the credential and
invalidates all prior device keys and sessions.

## Replay protection still required

AEAD authenticates a message but does not itself prevent replay. Before route
integration is considered complete, the backend must persist a per-device
monotonic request sequence (or unique nonce ledger) and reject repeats and
out-of-window values transactionally. Responses must carry a request nonce or
sequence in their authenticated plaintext so the Switch can correlate them.
The existing event sequence is not a substitute for a transport request
counter.

## Deployment

Cloudflare can proxy the HTTP ciphertext without seeing its contents, but
port 80 remains susceptible to metadata observation and connection blocking.
Disable caching and all automatic body transformations for API paths. Do not
enable HTTP-to-HTTPS redirects or Cloudflare Access/JS challenges on the
device API unless the client explicitly implements them. Keep the origin
backend private and firewall it from direct public access.

A production deployment is not approved until route-level envelope enforcement,
Switch-side AES-GCM, nonce/replay persistence, server-key protection, bootstrap
provisioning, and cross-language known-answer tests are all complete.
