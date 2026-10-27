# Central Control Server (FastAPI)

This feature lives on `feature/central-control-server`; `main` is unchanged.

## Network and security model

- The Switch client intentionally uses plain HTTP for a trusted local network. Configure `base_url` with an `http://` LAN address. HTTPS and other URL schemes are rejected by the client.
- HTTP does not encrypt traffic. Device bearer tokens and API responses can be observed or modified by other devices with access to the LAN/Wi-Fi. Use this only on a trusted network; do not expose the API to the public internet or forward its port on your router.
- Device tokens are random and only SHA-256 hashes are stored server-side. A token is returned once at enrollment.
- The enrollment code is one-use. Rotate it by replacing the server database or using the admin enrollment-code endpoint; do not reuse it.
- Admin API requires a separate long random bearer token.
- Never upload the console PIN, SD-card contents, or Nintendo credentials. TOTP has been removed from this branch.
- If the server is unreachable during launch, the remote-enabled build denies entry (fail closed). There is no offline grace period yet. During a later in-app sync failure, existing local settings remain unchanged.
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

Make the FastAPI service reachable only from the trusted LAN on port 8000. Ensure the container/service binds to an address reachable by the Switch, and restrict access with the host firewall. Do not expose port 8000 to the public internet.

## Enroll a console

The enrollment code is used once, then consumed. Call the HTTP endpoint from a trusted computer on the same LAN:

```bash
curl --fail-with-body --silent --show-error \
  -H 'Content-Type: application/json' \
  -d '{"enrollment_code":"YOUR_ONE_TIME_CODE","device_name":"Living-room Switch","client_version":"1.0"}' \
  http://192.168.10.2:8000/api/v1/enroll
```

Replace `192.168.10.2` with the server's LAN address. Save the returned `device_id` and `device_token` privately. The token cannot be retrieved again from the server database. Do not paste it into issue trackers or chat.

## Configure the Switch client

The Switch client uses HTTP only and does not need a CA file. Build with a Switch libcurl package and `-DNX_PCTL_REMOTE_CONTROL=ON`. If the package is not installed, keep the option off; the original build remains independent of it.

Create `sd:/switch/nx_pctl_server.conf` as plain key/value text:

```ini
base_url=http://192.168.10.2:8000
device_id=DEVICE_ID_FROM_ENROLLMENT
device_token=DEVICE_TOKEN_FROM_ENROLLMENT
apply_policy=0
```

Replace the example IP with the server's LAN IP. Set `apply_policy=1` only after testing. The token is a bearer credential; anyone with SD-card or LAN traffic access may be able to copy it. If compromised, disable the device in the admin API and provision a replacement.

## API

- `GET /health` — health check.
- `POST /api/v1/enroll` — one-time device enrollment.
- `GET /api/v1/auth/check` — authenticated gate check for the Switch client.
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
  http://192.168.10.2:8000/api/v1/admin/devices/DEVICE_ID/policy
```

The API currently provides a minimal JSON API, not a web dashboard. Time fields are stored/validated for future client support; the current Switch client does not apply bedtime values. Verify actual Switch behavior locally before relying on centrally configured limits.
