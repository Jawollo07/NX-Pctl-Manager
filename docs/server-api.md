# NX-Pctl-Manager HTTP API

The backend implements the server side of the device protocol. All device
operations use an authenticated encrypted envelope over HTTP:
`POST /v1/secure/{device_id}/{operation}`. Legacy plaintext device routes are
disabled by default. The headless Switch sysmodule contains the matching HTTP transport, AES-GCM/HKDF envelope client, enrollment/authentication flow, token refresh, heartbeat, policy retrieval and acknowledgement. The end-to-end system is still experimental: cross-language fixtures, firmware-specific hardware validation, credential recovery and enforcement gates must pass before public deployment.

## Run locally

Python 3.11+ is recommended.

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r server/requirements.txt
export NXPCTL_DATABASE="$PWD/data/nxpctl.sqlite3"
uvicorn server.app:app --host 127.0.0.1 --port 8080
```

Health check: `GET /healthz`. It contains no device secrets and is intentionally
not encrypted.

## Encrypted device API

Every device operation is an HTTP POST with the envelope specified in
[secure-http-protocol.md](secure-http-protocol.md). The outer envelope carries
version, a random request identifier, a SHA-256 secret key selector, a random
GCM nonce and ciphertext. The key selector is not a secret; the underlying
invitation code or device credential is.

Operations:

| Operation | Path | Encrypted plaintext |
|---|---|---|
| Enrollment | `/v1/secure/{id}/enroll` | `{"enrollment_code":"..."}` |
| Authentication | `/v1/secure/{id}/auth` | `{"device_credential":"..."}` |
| Policy | `/v1/secure/{id}/policy` | `{"access_token":"..."}` |
| Heartbeat | `/v1/secure/{id}/heartbeat` | Token, uptime/policy revision and optional global daily usage (`global_usage_day_key`, `global_usage_active_seconds`) |
| Events | `/v1/secure/{id}/events` | Token plus `events` array |
| Policy ack | `/v1/secure/{id}/policy-ack` | Token plus revision/status/message |

The HTTP path, method, device ID, direction and random request identifier are
bound into AES-GCM associated data. Responses use the same request identifier
and a response-direction AAD value. Access tokens are carried inside the
encrypted plaintext, never in the Authorization header.

### Enrollment bootstrap

Create a one-time invitation locally:

```sh
python -m server.cli create-enrollment --device-id switch-livingroom
```

The CLI prints the invitation code once. Provision it to the Switch through a
trusted local process. The Switch hashes the code with SHA-256, derives the
enrollment AES key using HKDF-SHA-256, and sends the encrypted enrollment
request. The SHA-256 digest is sent as the non-secret `k` key selector. The
backend looks up the outstanding invitation hash and derives the same key.
The newly generated device credential is returned only inside the encrypted
response. The database stores only its SHA-256 hash.

### Authentication and device key

The Switch hashes the device credential and derives the device AES key. The
encrypted authentication operation exchanges the credential for a bearer
token with a 15-minute lifetime. The backend stores only the token hash. All
subsequent device requests include the bearer token inside the encrypted
payload. The token must remain in Switch RAM and be refreshed by another
encrypted auth request after expiry.

### Replay protection

The Switch generates a fresh random 64-bit request identifier for every
request; no persistent counter is required. The backend atomically stores
identifiers in SQLite, scoped to the credential or access-token epoch, and
rejects duplicates. Authentication replay identifiers are retained for the
lifetime of the device credential. Session identifiers are pruned after the
15-minute token lifetime plus a short grace period. Enrollment identifiers
are pruned after the invitation lifetime. A random collision returns HTTP 409;
the client retries with a new identifier. The 96-bit GCM nonce must also be
fresh for every encryption under a given key.

## Local compatibility endpoints

The original plaintext endpoints under `/v1/devices/*` are disabled by
default. They exist only for isolated development and backward-compatibility
tests. To enable them explicitly:

```sh
NXPCTL_ALLOW_LEGACY_API=1 uvicorn server.app:app --host 127.0.0.1 --port 8080
```

Never enable this setting on an untrusted network or on the Cloudflare public
hostname. A request to these routes returns 404 by default.

## Policy administration

Create a JSON file matching [policy-schema-v1.md](policy-schema-v1.md), then:

```sh
python -m server.cli set-policy --device-id switch-livingroom --file policy.json
python -m server.cli list-devices  # includes the latest encrypted heartbeat and global usage snapshot
python -m server.cli revoke-device --device-id switch-livingroom
```

The CLI validates the exact schema, device binding, time bounds, title rules,
limits and monotonically increasing policy revision. Revocation invalidates
all bearer tokens. Re-enrollment requires a new one-time invitation and
rotates the device credential, policy epoch, event history and acknowledgements.

## Cloudflare deployment

Cloudflare may proxy the HTTP ciphertext, but HTTP metadata remains visible:
IP addresses, hostname, URL path, headers, packet sizes and timing. Cloudflare
can block, delay or replay traffic; authenticated encryption and replay
rejection protect message contents and authenticity, not availability or
metadata privacy.

For device API paths:
- Disable caching and body transformations.
- Do not apply HTTP-to-HTTPS redirects or interactive Cloudflare Access,
  browser challenges or JavaScript challenges.
- Keep the origin service bound to a private/loopback listener and firewall
  direct public access.
- Do not expose the legacy plaintext routes.

## Persistence and current limitations

The encrypted heartbeat stores the latest reported global daily usage day key and active seconds in `devices.last_heartbeat_json`; it is a single console-wide counter, not a per-title breakdown. The Switch sends it on the normal heartbeat cadence (about once per minute) only after the trusted day is available.\n\nSQLite stores device credential hashes, enrollment-code hashes, access-token
hashes, policies, events, acknowledgements and secure replay identifiers.
Raw credentials, invitation codes and bearer tokens are never stored.
Protect and back up the database directory.

The Python backend now implements encrypted device routes and replay
protection. The Switch-side AES-GCM/HKDF implementation, HTTP client, secure enrollment, token refresh and worker integration are present in this branch. Remaining gates include automated cross-language known-answer/fixture coverage, real-device transport and save-data validation, credential recovery, and policy enforcement. Until those gates pass, do not expose the API publicly over HTTP.


## Backend tests

Run the backend regression suite from the repository root:

```sh
python -m pip install -r server/requirements.txt
python -m unittest discover -s server/tests -v
```

The GitHub Actions workflow `.github/workflows/server-tests.yml` runs the same
suite when backend files change on the migration branch or in a pull request.
