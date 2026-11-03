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


## Cloudflare-proxied deployment (intended production topology)

The Switch client is being designed to connect to the **public HTTPS hostname**,
not to the origin address. Example topology:

```text
Nintendo Switch
  └─ HTTPS :443 / api.example.net
       └─ Cloudflare proxy (orange-cloud DNS, edge certificate)
            └─ HTTPS :443 (Full strict, origin certificate)
                 └─ reverse proxy on the Ubuntu host
                      └─ HTTP 127.0.0.1:8080
                           └─ nxpctl-api container
```

Replace `api.example.net` with the hostname you actually configure. Do not
hard-code an origin IP in the Switch client: proxied DNS resolves to Cloudflare
anycast addresses, and Cloudflare may change those addresses.

### Cloudflare and reverse-proxy settings

- Set the API A/AAAA/CNAME record to **Proxied** (orange cloud).
- Set SSL/TLS encryption to **Full (strict)**. The reverse proxy must present
  an unexpired certificate whose SAN matches the origin hostname; a publicly
  trusted certificate or Cloudflare Origin CA certificate is suitable for the
  Cloudflare-to-origin leg.
- Keep the application container bound to loopback (the Compose default
  `127.0.0.1:8080`). Terminate TLS at the reverse proxy on port 443 and
  proxy only the API path to the container.
- The Switch validates the **Cloudflare edge certificate chain and hostname**.
  It must not be configured to trust a Cloudflare Origin CA certificate as the
  server certificate: that certificate is presented only by the origin to
  Cloudflare, not by the Cloudflare edge to the Switch.
- Do not use Flexible SSL, HTTP, or a redirect from HTTP as the device API
  transport. Enrollment codes, device credentials, bearer tokens and policy
  responses are sensitive.
- Use the standard HTTPS port 443. Cloudflare also proxies selected alternate
  HTTPS ports, but there is no benefit in adding that complexity here.
- Disable caching for `/v1/*` and `/healthz`; send/retain
  `Cache-Control: no-store` for device-specific responses. Do not let a
  cache serve one device's policy to another.
- Do not put Cloudflare Access browser login/challenge pages in front of the
  device API. Device authentication is the backend's per-device bearer-token
  scheme. A browser challenge is not an API response and cannot be completed
  by the headless sysmodule.
- Apply rate limits/WAF rules to enrollment and authentication endpoints while
  allowing the device API's JSON POST/GET methods. Avoid interactive
  JavaScript or managed challenge actions on `/v1/*`.

Cloudflare's standard proxy accepts HTTPS on port 443 and selected alternate
ports; the origin connection is separately protected by the chosen SSL/TLS
mode. See the official [Cloudflare proxied network ports
documentation](https://developers.cloudflare.com/fundamentals/reference/network-ports/)
and [Full (strict) mode
documentation](https://developers.cloudflare.com/ssl/origin-configuration/ssl-modes/full-strict/).

### Trust boundary

TLS from the Switch to Cloudflare protects credentials and policy data in
transit and authenticates the public API hostname, provided the Switch TLS
implementation performs normal certificate-chain and hostname validation.
Cloudflare then establishes a separate TLS connection to the origin in Full
(strict) mode. This is transport security, not end-to-end signing: Cloudflare
terminates TLS and can technically inspect or modify proxied traffic. If
cryptographic integrity against the proxy itself is later required, policy
snapshots need a separate signed format and a pinned verification public key
on the Switch.

The request builder and response parser are protocol framing helpers only.
They do not open sockets, perform TLS, validate certificates, follow redirects,
or authenticate response content. No server data is trusted until those
transport and policy-authentication steps exist.
