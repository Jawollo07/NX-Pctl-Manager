# Backend deployment

The backend is the authoritative control plane. The Switch communicates with
`/v1/secure/*` using authenticated AES-GCM envelopes over HTTP. The browser
dashboard is served at `/admin` and uses the administrator bearer token.
Expose the dashboard only through HTTPS; do not expose the origin port publicly.

## Docker Compose

Run these commands from the repository root on the Ubuntu host:

```sh
cp server/.env.example server/.env
python3 - <<'PY'
from pathlib import Path
import secrets

path = Path("server/.env")
path.write_text(
    "NXPCTL_ADMIN_TOKEN=" + secrets.token_urlsafe(48) + "\n"
    "NXPCTL_PARENT_AUTH_KEY=" + secrets.token_hex(32) + "\n",
    encoding="ascii",
)
path.chmod(0o600)
PY
docker compose -f server/compose.yaml up -d --build
docker compose -f server/compose.yaml ps
curl --fail http://127.0.0.1:8080/healthz
```

The `.env.example` file is a template only. Never commit `server/.env`.
The compose service binds to loopback port 8080 and stores SQLite data in the
named `nxpctl-data` volume. It runs read-only apart from the persistent data
volume and temporary filesystem.

Open `https://YOUR-HOSTNAME/admin` after configuring a reverse proxy with a
valid certificate. If the proxy runs directly on the Ubuntu host, its upstream
is `http://127.0.0.1:8080`. The compose port is deliberately bound to loopback.

If the reverse proxy runs in another Docker container (for example, Nginx
Proxy Manager), that container cannot reach the host's `127.0.0.1`. Connect
both containers to a dedicated shared Docker network and use the Compose
service name `nxpctl-api:8080`, or explicitly configure a host-gateway route
for the proxy container. Do not solve this by publishing port 8080 on
`0.0.0.0`. The reverse proxy must forward the original request path and
request body unchanged.
Disable caching, body rewriting, browser challenges and interactive access
gates on `/v1/secure/*`; the Switch client cannot complete browser-based
authentication challenges. Do not expose port 8080 directly to the Internet.

The application envelope encrypts and authenticates device API bodies. It
does not hide IP addresses, hostnames, paths, headers, packet sizes or timing.
The dashboard bearer token is not protected by the device envelope, so browser
administration must use HTTPS end-to-end to the trusted reverse-proxy edge.
Do not configure a blanket HTTP redirect or Cloudflare browser challenge on
the device API path if the configured Switch endpoint uses HTTP.

## Secrets and recovery

- `NXPCTL_ADMIN_TOKEN`: administrator API/dashboard credential.
- `NXPCTL_PARENT_AUTH_KEY`: AES-256-GCM key protecting TOTP seeds in SQLite.
  Losing this key makes existing TOTP seeds unrecoverable.
- Back up `server/.env` separately from the SQLite volume. Restrict it to the
  service administrator (`chmod 600`).
- Back up SQLite using SQLite's online backup API or while the service is
  stopped. Do not copy only the main database file while WAL writes are active.
- Keep backups encrypted and test restoring both the database and key file.

To inspect service logs:

```sh
docker compose -f server/compose.yaml logs --tail=200 -f nxpctl-api
```

To upgrade after pulling the intended feature branch:

```sh
docker compose -f server/compose.yaml up -d --build
```

## Operational limitations

This is an experimental project. The dashboard/API and CI build do not prove
that the sysmodule works on a physical console. Before enabling enforcement,
validate the exact Atmosphere/Horizon combination, enrollment bootstrap,
persistent credential and usage storage, sleep/wake accounting, global process
termination, offline behavior, and a tested recovery/uninstall procedure.
