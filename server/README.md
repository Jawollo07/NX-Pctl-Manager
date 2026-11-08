# NX-Pctl-Manager backend

FastAPI control plane with SQLite server-side storage. Device enrollment,
global policy management, backend-managed parent authentication, revocation,
usage inspection and event logs are managed through authenticated APIs. The
Switch sysmodule is an execution/enforcement agent, not an administration UI.

## Run locally

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r server/requirements.txt
export NXPCTL_DATABASE="$PWD/server/data/nxpctl.sqlite3"
export NXPCTL_ADMIN_TOKEN="$(python -c 'import secrets; print(secrets.token_urlsafe(48))')"
export NXPCTL_PARENT_AUTH_KEY="$(python -c 'import secrets; print(secrets.token_hex(32))')"
uvicorn server.app:app --host 127.0.0.1 --port 8080
```

Open `http://127.0.0.1:8080/admin` for local-only development. For remote
administration use the Docker Compose deployment in
[../docs/deployment.md](../docs/deployment.md) and put the dashboard behind
HTTPS. Never expose the origin port directly.

Keep both secrets outside version control. Back up
`NXPCTL_PARENT_AUTH_KEY` separately from SQLite: losing it makes stored TOTP
seeds unrecoverable. The dashboard keeps the administrator token in page
memory only; it does not write it to localStorage or cookies.

Device API request/response bodies use authenticated AES-GCM envelopes over
HTTP; HTTP metadata remains visible. Do not cache, rewrite, challenge or
redirect the encrypted device paths. Hardware validation is still required.
