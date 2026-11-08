from __future__ import annotations

import base64
import hashlib
import time

import pytest
from fastapi.testclient import TestClient

from server import app as api
from server.parent_auth_api import _code, issue_or_read_grant


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("NXPCTL_DATABASE", str(tmp_path / "parent-auth.sqlite3"))
    monkeypatch.setenv("NXPCTL_ALLOW_LEGACY_API", "1")
    monkeypatch.setenv("NXPCTL_ADMIN_TOKEN", "admin-token-" + "x" * 40)
    monkeypatch.setenv("NXPCTL_PARENT_AUTH_KEY", "42" * 32)
    api.init_database()
    with TestClient(api.app) as test_client:
        with api.database() as db:
            db.execute(
                "INSERT INTO devices(device_id,credential_hash,created_at) VALUES(?,?,?)",
                ("switch-parent", hashlib.sha256(b"credential").hexdigest(), int(time.time())),
            )
        yield test_client


def headers():
    return {"Authorization": "Bearer " + "admin-token-" + "x" * 40}


def setup_auth(client):
    response = client.post(
        "/v1/admin/devices/switch-parent/parent-auth",
        headers=headers(), json={"confirm": True},
    )
    assert response.status_code == 200, response.text
    return response.json()["totp_secret"]


def test_parent_auth_secret_is_backend_managed_and_not_returned_by_status(client):
    secret_text = setup_auth(client)
    secret = base64.b32decode(secret_text + "=" * ((8 - len(secret_text) % 8) % 8))
    with api.database() as db:
        row = db.execute(
            "SELECT secret_nonce,secret_ciphertext FROM parent_auth WHERE device_id=?",
            ("switch-parent",),
        ).fetchone()
    assert bytes(row["secret_ciphertext"]) != secret
    assert secret not in bytes(row["secret_ciphertext"])
    status = client.get("/v1/admin/devices/switch-parent/parent-auth", headers=headers())
    assert status.status_code == 200
    assert status.json()["configured"] is True
    assert "totp_secret" not in status.json()


def test_parent_auth_totp_creates_short_lived_grant_and_rejects_replay(client):
    secret_text = setup_auth(client)
    secret = base64.b32decode(secret_text + "=" * ((8 - len(secret_text) % 8) % 8))
    code = _code(secret, int(time.time()) // 30)
    response = client.post(
        "/v1/admin/devices/switch-parent/parent-auth/verify",
        headers=headers(), json={"code": code},
    )
    assert response.status_code == 200, response.text
    assert response.json()["authenticated"] is True
    grant = issue_or_read_grant("switch-parent")
    assert grant["parent_authenticated"] is True
    assert grant["authenticated_until"] > int(time.time())

    replay = client.post(
        "/v1/admin/devices/switch-parent/parent-auth/verify",
        headers=headers(), json={"code": code},
    )
    assert replay.status_code == 401


def test_parent_auth_requires_admin_and_locks_after_five_failures(client):
    assert client.post(
        "/v1/admin/devices/switch-parent/parent-auth",
        json={"confirm": True},
    ).status_code == 401
    setup_auth(client)
    for attempt in range(5):
        response = client.post(
            "/v1/admin/devices/switch-parent/parent-auth/verify",
            headers=headers(), json={"code": "000000"},
        )
    assert response.status_code == 429
    assert issue_or_read_grant("switch-parent")["parent_authenticated"] is False


def test_parent_auth_can_be_revoked(client):
    setup_auth(client)
    response = client.delete(
        "/v1/admin/devices/switch-parent/parent-auth", headers=headers(),
    )
    assert response.status_code == 200
    assert response.json()["revoked"] is True
    assert issue_or_read_grant("switch-parent")["parent_authenticated"] is False
