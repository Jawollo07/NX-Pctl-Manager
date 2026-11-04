# NX-Pctl-Manager backend

FastAPI reference service with SQLite server-side storage. The backend provides
device enrollment/authentication, policy delivery, heartbeats, event batches
and policy acknowledgements.

See [the API and deployment guide](../docs/server-api.md) for complete setup,
endpoint examples, HTTP security notes and CLI usage.

## Quick start

```sh
python -m venv .venv
. .venv/bin/activate
pip install -r server/requirements.txt
export NXPCTL_DATABASE="$PWD/data/nxpctl.sqlite3"
uvicorn server.app:app --host 127.0.0.1 --port 8080
```

The CLI is run from the repository root:

```sh
python -m server.cli create-enrollment --device-id switch-livingroom
python -m server.cli set-policy --device-id switch-livingroom --file policy.json
python -m server.cli list-devices
python -m server.cli revoke-device --device-id switch-livingroom
```

Device requests use the encrypted AES-GCM envelope over HTTP; HTTP metadata remains visible. Do not expose the origin directly. For untrusted networks, place it behind the configured reverse proxy and ensure device API paths are not redirected, cached, transformed, or challenged. The current headless sysmodule contains the matching HTTP client and secure synchronization worker, but the end-to-end deployment still requires firmware-specific hardware validation.
