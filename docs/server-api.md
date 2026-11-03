# NX-Pctl-Manager HTTP API

Status: the reference backend is implemented in `server/`. The Switch-side
HTTP transport, enrollment UI/provisioning flow and policy publication into
the running sysmodule are **not implemented yet**.

## Run locally

Python 3.11+ is recommended (CI uses 3.12).

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r server/requirements.txt
export NXPCTL_DATABASE="$PWD/data/nxpctl.sqlite3"
uvicorn server.app:app --host 127.0.0.1 --port 8080
```

Health check: `GET http://127.0.0.1:8080/healthz`.

The default bind address is loopback. For a remote Switch, put the service
behind a firewall and a TLS reverse proxy, or explicitly expose the HTTP
listener only on a trusted, isolated LAN. **Plain HTTP exposes enrollment
codes, device credentials, bearer tokens, policies and telemetry to anyone
able to observe or modify that network traffic.** HTTP is supported for
development and trusted private networks, not presented as secure transport.
Do not forward port 8080 directly to the public internet.

## Docker Compose

```sh
cd server
docker compose up -d --build
```

The service binds to host loopback port 8080 and stores SQLite data in the
persistent `nxpctl-data` volume. Put a TLS reverse proxy in front if clients
connect over an untrusted network. Back up the volume regularly.

## Device enrollment

Create a one-time invitation locally on the server:

```sh
python -m server.cli create-enrollment --device-id switch-livingroom
```

The CLI prints a random enrollment code once. Give it to the device through a
trusted provisioning path. The Switch calls:

```http
POST /v1/devices/enroll
Content-Type: application/json

{"device_id":"switch-livingroom","enrollment_code":"ONE_TIME_CODE"}
```

The response contains a random per-device `device_credential`. It is returned
only once; the database stores only its SHA-256 hash. Provisioning and secure
persistent storage of that credential on Horizon are still a separate,
unimplemented Switch-side task. Never commit credentials or invitation codes.

The device exchanges the credential for a short-lived bearer token:

```http
POST /v1/devices/auth
Content-Type: application/json

{"device_id":"switch-livingroom","device_credential":"DEVICE_CREDENTIAL"}
```

The response contains `access_token`, `expires_at`, `device_id` and
`server_time`. Tokens expire after 15 minutes. Only their SHA-256 hashes are
stored. The device must keep the raw token in RAM and authenticate again after
expiry.

## Device endpoints

All endpoints below except health and enrollment require
`Authorization: Bearer ACCESS_TOKEN`. A token is scoped to exactly one device;
the path device ID must match its authenticated subject.

| Method | Endpoint | Purpose |
|---|---|---|
| GET | `/healthz` | Liveness |
| POST | `/v1/devices/enroll` | Consume one-time invitation |
| POST | `/v1/devices/auth` | Exchange device credential for token |
| GET | `/v1/devices/{id}/policy` | Fetch complete raw policy JSON |
| POST | `/v1/devices/{id}/heartbeat` | Store bounded health snapshot |
| POST | `/v1/devices/{id}/events:batch` | Store/deduplicate event batch |
| POST | `/v1/devices/{id}/policy-ack` | Record policy processing result |

Event batches accept 1–100 events. Each event has exactly
`event_id`, `sequence`, `timestamp`, `type` and `payload`. Event IDs and
per-device sequence numbers are unique; retries are safely deduplicated.
Payloads are limited to 4 KiB each. Request bodies are limited to 64 KiB.

Heartbeat fields are intentionally forward-compatible, with protocol version,
uptime and policy revision range-checked when present. The service does not
accept remote shell commands or arbitrary code.

## Publish a policy

Create a JSON file matching the exact schema in
[`policy-schema-v1.md`](policy-schema-v1.md), then run:

```sh
python -m server.cli set-policy --device-id switch-livingroom --file policy.json
```

The CLI overwrites `server_time` with the current server clock, validates all
18 required fields, title rules, schedule bounds, device binding and validity
interval, and rejects a revision that is not newer than the stored one. The
policy endpoint returns the raw JSON object (not an envelope). The backend
does not claim the policy is authenticated end-to-end to the Switch over
plaintext HTTP.

## Administration

```sh
python -m server.cli list-devices
python -m server.cli revoke-device --device-id switch-livingroom
```

Revocation invalidates all active bearer tokens immediately. Re-enrollment
requires an administrator to create a new invitation after revocation. Re-enrollment rotates the credential and clears the previous policy, tokens, event epoch and acknowledgements.

The backend intentionally has no unauthenticated browser-admin API. Device
provisioning, revocation and policy publishing are local CLI operations.

## Persistence and security boundaries

- SQLite is server-side authoritative storage for devices, token hashes,
  policy snapshots, heartbeats, events and policy acknowledgements.
- Raw device credentials, enrollment codes and access tokens are never stored.
- Policy documents are strictly parsed; duplicate JSON object keys, floats,
  unknown/missing policy fields, cross-device policy and invalid ranges fail.
- No policy/usage/event database is added to the Switch.
- Database access is local to the service. Protect the data directory and
  include it in server backup/restore procedures.
- The backend currently supports HTTP. TLS termination can be provided by a
  reverse proxy without changing the API contract. The Switch-side transport
  must still validate server identity before treating responses as trusted.

## Current implementation boundary

The backend routes and CLI exist and are covered by automated tests. There is
not yet a Switch HTTP client, secure device-credential provisioning/storage,
automatic authentication/refresh, policy fetch scheduling, event queue, or
worker integration. Until those are implemented and tested, the sysmodule does
not contact this server and no policy is enforced on-console.
