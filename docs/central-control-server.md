# Central Control Server (FastAPI)

This feature lives on `feature/central-control-server`; `main` is unchanged.

## Security model

- Serve the API behind a reverse proxy with a valid public HTTPS certificate. Do not expose port 8000 directly.
- Device tokens are random and only SHA-256 hashes are stored server-side. A token is returned once at enrollment.
- The enrollment code is one-use. Rotate it by replacing the server database or adding a future admin rotation endpoint; do not reuse it.
- Admin API requires a separate long random bearer token.
- Never upload the console PIN, TOTP secret, SD-card contents, or Nintendo credentials.
- Offline/unreachable server: the client leaves local settings unchanged.
- A server policy is not an arbitrary command. The current Switch client only recognizes the optional daily limit.
- Automatic policy application is opt-in per console via `apply_policy=1`; default is off. It only attempts a daily-limit write when a PIN exists and the firmware supports the known play-timer API.

## Run the API on Ubuntu

```bash
cd server
cp .env.example .env
python3 -c 'import secrets; print(secrets.token_urlsafe(32))'
# Put separate random values in CONTROL_ADMIN_TOKEN and CONTROL_ENROLLMENT_CODE.
mkdir -p data
docker compose up -d --build
```

Configure Caddy/nginx/your reverse proxy to forward HTTPS requests to the container's port 8000. The container port is internal-only in compose. Protect the domain with TLS and keep the admin token private.

## Enroll a console

The enrollment code is used once, then consumed. Call the public HTTPS endpoint from a trusted admin machine:

```bash
curl --fail-with-body --silent --show-error \
  -H 'Content-Type: application/json' \
  -d '{"enrollment_code":"YOUR_ONE_TIME_CODE","device_name":"Living-room Switch","client_version":"1.0"}' \
  https://YOUR_DOMAIN/api/v1/enroll
```

Save the returned `device_id` and `device_token` privately. The token cannot be retrieved again from the server database. Do not paste it into issue trackers or chat.

## Configure the Switch client

The Switch-side HTTPS transport uses libcurl with certificate and hostname validation enabled. Install the devkitPro Switch libcurl package in your build environment, then configure CMake with `-DNX_PCTL_REMOTE_CONTROL=ON`. If the package is not installed, keep the option off; the original build remains independent of it.

Create `sd:/switch/nx_pctl_server.conf` as plain key/value text:

```ini
base_url=https://YOUR_DOMAIN
device_id=DEVICE_ID_FROM_ENROLLMENT
device_token=DEVICE_TOKEN_FROM_ENROLLMENT
apply_policy=0
```

Set `apply_policy=1` only after testing. The token is a bearer credential; anyone with SD-card access can copy it. If compromised, disable the device in the admin API and provision a replacement.

## API

- `GET /health` — health check.
- `POST /api/v1/enroll` — one-time device enrollment.
- `GET /api/v1/policy` — authenticated policy fetch.
- `POST /api/v1/status` — authenticated status report.
- `GET /api/v1/admin/devices` — admin-only device list.
- `PUT /api/v1/admin/devices/{device_id}/policy` — admin-only policy update.
- `POST /api/v1/admin/devices/{device_id}/disable` — revoke a device token.
- `POST /api/v1/admin/enrollment-code` — admin-only, generate a new one-time enrollment code (returned once).

Admin requests use `Authorization: Bearer $CONTROL_ADMIN_TOKEN`.

Example policy update:

```bash
curl --fail-with-body -X PUT \
  -H "Authorization: Bearer $CONTROL_ADMIN_TOKEN" \
  -H 'Content-Type: application/json' \
  -d '{"enabled":true,"daily_limit_minutes":90,"note":"weekday limit"}' \
  https://YOUR_DOMAIN/api/v1/admin/devices/DEVICE_ID/policy
```

The API currently provides a minimal JSON API, not a web dashboard. Time fields are stored/validated for future client support; the current Switch client does not apply bedtime values. Verify actual Switch behavior locally before relying on centrally configured limits.
