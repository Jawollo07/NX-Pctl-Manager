# NX-Pctl-Manager backend

FastAPI control plane with SQLite server-side storage. Device enrollment,
global policy management, parent authentication, revocation, usage inspection,
and event logs are managed through authenticated backend APIs. The Switch
sysmodule is an execution/enforcement agent, not an administration interface.

See [the API and deployment guide](../docs/server-api.md) for endpoint
examples, key management, HTTP security notes and the remaining integration
gates.

## Quick start

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r server/requirements.txt
export NXPCTL_DATABASE="$PWD/data/nxpctl.sqlite3"
export NXPCTL_ADMIN_TOKEN="$(python -c 'import secrets; print(secrets.token_urlsafe(48))')"
export NXPCTL_PARENT_AUTH_KEY="$(python -c 'import secrets; print(secrets.token_hex(32))')"
uvicorn server.app:app --host 127.0.0.1 --port 8080
```

Open `http://127.0.0.1:8080/admin` to use the browser dashboard. The dashboard asks for the administrator token on each visit and keeps it in page memory only; it does not store it in localStorage or cookies. Put the dashboard behind an authenticated HTTPS reverse proxy before remote use.\n\nKeep both secrets in a protected secret manager or environment file. Back up
`NXPCTL_PARENT_AUTH_KEY` separately from SQLite: losing it makes stored TOTP
seeds unrecoverable. Never commit either secret.

The local CLI remains available for recovery and isolated development, but
normal management is intended to use the backend API. Device requests use the
encrypted AES-GCM envelope over HTTP; HTTP metadata remains visible. Do not
expose the origin directly. Place it behind the configured reverse proxy and
ensure device API paths are not redirected, cached, transformed, or challenged.
The end-to-end deployment still requires firmware-specific hardware validation.