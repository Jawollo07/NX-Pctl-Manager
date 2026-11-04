# End-to-end encrypted HTTP protocol v1

## Security boundary

Device requests and responses use AES-128-GCM envelopes over HTTP. Encryption
and authentication are end-to-end between the Switch and backend; Cloudflare
does not receive the key. This protects message bodies, not IP addresses,
hostnames, URL paths, HTTP headers, packet sizes, timing, availability or
traffic analysis.

FastAPI implements `POST /v1/secure/{device_id}/{operation}`. Legacy
plaintext `/v1/devices/*` routes are disabled unless
`NXPCTL_ALLOW_LEGACY_API=1` is explicitly set for isolated development.
The Switch-side HTTP client and matching AES-GCM/HKDF implementation are present. Shared C++/Python known-answer fixtures and encrypted API integration tests pass. Hardware connectivity, deployment configuration and platform enforcement are still unverified; do not treat the current build as production-ready.

## Wire envelope

Every secure endpoint accepts and returns exactly this outer JSON shape:

```json
{"v":1,"s":42,"k":"64_LOWERCASE_HEX_CHARS","n":"BASE64URL_NONCE","c":"BASE64URL_CIPHERTEXT_AND_TAG"}
```

- `v`: protocol version 1.
- `s`: fresh random 63-bit request identifier (kept within SQLite's signed INTEGER range). The Switch generates
  a new cryptographically random value for each request; it is not a counter
  and requires no persistent storage. The response echoes it.
- `k`: SHA-256 digest of the invitation code or device credential, encoded as
  64 lowercase hex characters. This is a key selector, not a secret. The
  high-entropy original secret is never sent outside ciphertext.
- `n`: fresh 96-bit random GCM nonce, Base64url without padding.
- `c`: ciphertext followed by the 128-bit GCM tag, Base64url without padding.
- Maximum plaintext is 64 KiB; the encoded HTTP envelope is bounded by the
  backend's 96 KiB request limit.

AES-128-GCM uses a 16-byte key. The associated data is the ASCII encoding of:

```text
NX-PCTL-AEAD/1
{direction}
{method}
{path}
{device_id}
{sequence}
```

The fields are separated by single LF bytes (no CR). Direction is
`request` or `response`; method is always `POST`; path is the exact
`/v1/secure/{device_id}/{operation}` path. This binds ciphertext to its
operation, device, direction and request identifier. AAD is not encrypted,
but is authenticated.

## Key derivation

1. Compute `secret_digest = SHA256(secret_utf8)`.
2. HKDF-Extract with HMAC-SHA-256:
   `PRK = HMAC-SHA256(key="NX-PCTL-AEAD-1", data=secret_digest)`.
3. HKDF-Expand for one block:
   `OKM = HMAC-SHA256(key=PRK, data=info || 0x01)`.
4. Use the first 16 bytes of OKM as the AES-128 key.

The exact ASCII `info` string is
`NX-PCTL-AEAD/1/{purpose}/{device_id}`; purpose is `enrollment` or
`device`. The backend derives from the already stored SHA-256 hash, so raw
credentials do not need to be stored recoverably.

## Operations

All operations are HTTP POST. The access token is inside the encrypted JSON
plaintext, never in an Authorization header.

| Operation | Encrypted request plaintext |
|---|---|
| `enroll` | `{"enrollment_code":"..."}` |
| `auth` | `{"device_credential":"..."}` |
| `policy` | `{"access_token":"..."}` |
| `heartbeat` | `{"access_token":"...","protocol_version":1,...}` |
| `events` | `{"access_token":"...","events":[...]}` |
| `policy-ack` | `{"access_token":"...","revision":1,"status":"accepted"}` |

### Enrollment bootstrap

The Switch receives the one-time invitation code through a trusted local
provisioning path. It computes its SHA-256 digest, derives the enrollment key,
and encrypts the code in the request body. The digest is included as `k` so
the backend can locate the outstanding invitation and derive the same key.
After decrypting and verifying the code, the backend returns the new random
device credential only inside the encrypted response. The invitation is
single-use.

### Authentication and device operations

The Switch derives the device key from the device credential. The encrypted
`auth` operation exchanges that credential for a 15-minute bearer token.
The token is stored only as a hash by the backend and is sent only inside
encrypted request bodies. Policy, heartbeat, event and policy-ack operations
use the same device key and include the bearer token in their encrypted
plaintext.

## Replay protection

AES-GCM alone does not prevent replay. The backend atomically claims each random request identifier in SQLite, scoped to the credential or session key epoch. Once a response is committed, a retry of the same request identifier receives the exact cached encrypted response and HTTP status without executing the operation again. A concurrent duplicate while the original is still processing receives HTTP 409. Authentication identifiers are retained for the lifetime of the credential. Enrollment identifiers are pruned after the invitation lifetime. Session identifiers are pruned after the token TTL plus a grace period; an expired token is independently rejected. The client must reuse an identifier only when retrying the byte-identical encrypted request. The GCM nonce must be fresh for every new encryption under a key.

## Deployment

Cloudflare may proxy the ciphertext, but sees metadata and can block or delay
requests. For device API paths, disable caching and body transformations, do
not apply HTTP redirects or interactive Access/browser challenges, and keep
the origin private. Do not enable the legacy plaintext API on the public
listener.

Production use remains blocked until real-device transport, secure random generation under Horizon, credential recovery, deployment hardening and enforcement integration have been validated. The current automated tests do not substitute for hardware tests.
