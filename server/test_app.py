from __future__ import annotations

import json
import time

import pytest
from fastapi import HTTPException
from fastapi.testclient import TestClient

from server import app as api
from server.secure_transport import MessageContext, derive_key, open_json, seal_json


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("NXPCTL_DATABASE", str(tmp_path / "test.sqlite3"))
    monkeypatch.setenv("NXPCTL_ALLOW_LEGACY_API", "1")
    api.init_database()
    with TestClient(api.app) as test_client:
        yield test_client


def enrollment_code(device_id="switch-test"):
    code = "enroll-" + ("x" * 32)
    with api.database() as db:
        # Mirror the CLI: only one outstanding enrollment code per device.
        db.execute("DELETE FROM enrollment_codes WHERE device_id=?", (device_id,))
        db.execute(
            "INSERT INTO enrollment_codes(code_hash,device_id,expires_at) VALUES(?,?,?)",
            (api._hash_secret(code), device_id, int(time.time()) + 600),
        )
    return code


def valid_policy(device_id="switch-test", revision=1):
    now = int(time.time())
    return {
        "protocol_version": 1,
        "schema_version": 1,
        "device_id": device_id,
        "revision": revision,
        "issued_at": now,
        "not_after": now + 3600,
        "server_time": now,
        "enabled": True,
        "require_parent_authentication": False,
        "offline_behavior": 0,
        "timezone_offset_minutes": 60,
        "global_daily_limit_minutes": 120,
        "weekday_limit_minutes": [65535] * 7,
        "quiet_hours_enabled": False,
        "quiet_hours_start_minute": 1320,
        "quiet_hours_end_minute": 420,
    }


def enroll_and_auth(client, device_id="switch-test"):
    response = client.post("/v1/devices/enroll", json={
        "device_id": device_id,
        "enrollment_code": enrollment_code(device_id),
    })
    assert response.status_code == 200, response.text
    credential = response.json()["device_credential"]
    auth = client.post("/v1/devices/auth", json={
        "device_id": device_id,
        "device_credential": credential,
    })
    assert auth.status_code == 200, auth.text
    return auth.json()["access_token"]


def test_enrollment_is_single_use_and_credential_is_not_returned_again(client):
    code = enrollment_code()
    first = client.post("/v1/devices/enroll", json={"device_id": "switch-test", "enrollment_code": code})
    assert first.status_code == 200
    assert len(first.json()["device_credential"]) >= 32
    second = client.post("/v1/devices/enroll", json={"device_id": "switch-test", "enrollment_code": code})
    assert second.status_code == 403


def test_auth_policy_and_device_scope(client):
    token = enroll_and_auth(client)
    headers = {"Authorization": f"Bearer {token}"}
    policy = valid_policy()
    with api.database() as db:
        db.execute(
            "INSERT INTO policies(device_id,revision,body_json,updated_at) VALUES(?,?,?,?)",
            ("switch-test", 1, json.dumps(policy), int(time.time())),
        )
    response = client.get("/v1/devices/switch-test/policy", headers=headers)
    assert response.status_code == 200
    assert response.json()["revision"] == 1
    assert abs(response.json()["server_time"] - int(time.time())) <= 2
    assert client.get("/v1/devices/another-device/policy", headers=headers).status_code == 403
    assert client.get("/v1/devices/switch-test/policy").status_code == 401


def test_revoked_device_cannot_use_existing_token(client):
    token = enroll_and_auth(client)
    with api.database() as db:
        db.execute("UPDATE devices SET revoked_at=? WHERE device_id=?", (int(time.time()), "switch-test"))
    response = client.post("/v1/devices/switch-test/heartbeat", headers={"Authorization": f"Bearer {token}"}, json={})
    assert response.status_code == 401


def test_events_are_deduplicated(client):
    token = enroll_and_auth(client)
    headers = {"Authorization": f"Bearer {token}"}
    event = {"event_id": "evt-1", "sequence": 1, "timestamp": int(time.time()), "type": "usage.sample", "payload": {"global_usage_active_seconds": 300}}
    response = client.post("/v1/devices/switch-test/events:batch", headers=headers, json={"events": [event, event]})
    assert response.status_code == 200
    assert response.json()["accepted"] == 1
    assert response.json()["received"] == 2


def test_events_reject_title_specific_tracking(client):
    token = enroll_and_auth(client)
    headers = {"Authorization": f"Bearer {token}"}
    title_event = {
        "event_id": "evt-title", "sequence": 2, "timestamp": int(time.time()),
        "type": "title.started", "payload": {"title_id": 123},
    }
    response = client.post(
        "/v1/devices/switch-test/events:batch", headers=headers,
        json={"events": [title_event]},
    )
    assert response.status_code == 422

    nested_title_event = {
        "event_id": "evt-title-nested", "sequence": 3, "timestamp": int(time.time()),
        "type": "usage.sample", "payload": {"details": {"title_name": "example"}},
    }
    response = client.post(
        "/v1/devices/switch-test/events:batch", headers=headers,
        json={"events": [nested_title_event]},
    )
    assert response.status_code == 422


def test_policy_validator_rejects_unknown_duplicate_or_cross_device_fields():
    policy = valid_policy()
    policy["unexpected"] = True
    with pytest.raises(Exception):
        api._validate_policy(policy, "switch-test")
    policy = valid_policy()
    policy["device_id"] = "other-device"
    with pytest.raises(Exception):
        api._validate_policy(policy, "switch-test")
    raw = b'{"device_id":"a","device_id":"b"}'
    with pytest.raises(api.InvalidJSON):
        api.strict_json_loads(raw)
    with pytest.raises(api.InvalidJSON):
        api.strict_json_loads(b'{"value":1.25}')
    with pytest.raises(api.InvalidJSON):
        api.strict_json_loads(b'{"a":{"b":{"c":{"d":{"e":{"f":{"g":{"h":{"i":1}}}}}}}}}')


def test_policy_validator_rejects_title_specific_fields():
    policy = valid_policy()
    policy["title_rules"] = []
    with pytest.raises(HTTPException):
        api._validate_policy(policy, "switch-test")

def test_revoked_device_can_be_reenrolled_with_rotated_credential(client):
    old_token = enroll_and_auth(client)
    with api.database() as db:
        db.execute("UPDATE devices SET revoked_at=? WHERE device_id=?", (int(time.time()), "switch-test"))
    code = enrollment_code()
    response = client.post("/v1/devices/enroll", json={"device_id": "switch-test", "enrollment_code": code})
    assert response.status_code == 200, response.text
    new_credential = response.json()["device_credential"]
    assert client.post("/v1/devices/auth", json={
        "device_id": "switch-test", "device_credential": new_credential,
    }).status_code == 200
    assert client.get("/v1/devices/switch-test/policy", headers={
        "Authorization": f"Bearer {old_token}",
    }).status_code == 401



def secure_post(client, device_id, operation, payload, secret, purpose="device", sequence=None):
    import secrets as _secrets
    from server.app import _hash_secret

    if sequence is None:
        sequence = _secrets.randbits(64)
    path = f"/v1/secure/{device_id}/{operation}"
    key = derive_key(secret.encode("utf-8"), device_id, purpose)
    context = MessageContext(device_id, "POST", path, "request", sequence)
    envelope = seal_json(payload, key, context)
    envelope["k"] = _hash_secret(secret)
    response = client.post(path, json=envelope)
    response_context = MessageContext(device_id, "POST", path, "response", sequence)
    if response.headers.get("content-type", "").startswith("application/json") and response.status_code != 400:
        try:
            response_envelope = response.json()
            assert response_envelope.get("k") == _hash_secret(secret)
            response_envelope.pop("k")
            decoded = open_json(response_envelope, key, response_context)
            return response, decoded
        except Exception:
            pass
    return response, None


def test_secure_enrollment_auth_and_heartbeat_are_encrypted(client, monkeypatch):
    monkeypatch.setenv("NXPCTL_ALLOW_LEGACY_API", "0")
    code = enrollment_code()
    enrolled, body = secure_post(
        client, "switch-test", "enroll", {"enrollment_code": code}, code, "enrollment",
    )
    assert enrolled.status_code == 200, enrolled.text
    assert body["device_id"] == "switch-test"
    credential = body["device_credential"]

    auth, auth_body = secure_post(
        client, "switch-test", "auth", {"device_credential": credential}, credential,
    )
    assert auth.status_code == 200, auth.text
    assert auth_body["token_type"] == "Bearer"

    heartbeat, heartbeat_body = secure_post(
        client, "switch-test", "heartbeat",
        {"access_token": auth_body["access_token"], "protocol_version": 1, "uptime_seconds": 17},
        credential,
    )
    assert heartbeat.status_code == 200, heartbeat.text
    assert heartbeat_body["accepted"] is True
    assert client.post("/v1/devices/switch-test/heartbeat", json={}).status_code == 404


def test_secure_enrollment_replay_returns_cached_credential(client, monkeypatch):
    import secrets as _secrets
    from server.app import _hash_secret

    monkeypatch.setenv("NXPCTL_ALLOW_LEGACY_API", "0")
    code = enrollment_code()
    sequence = _secrets.randbits(64)
    path = "/v1/secure/switch-test/enroll"
    key = derive_key(code.encode(), "switch-test", "enrollment")
    context = MessageContext("switch-test", "POST", path, "request", sequence)
    envelope = seal_json({"enrollment_code": code}, key, context)
    envelope["k"] = _hash_secret(code)
    first = client.post(path, json=envelope)
    assert first.status_code == 200
    replay = client.post(path, json=envelope)
    assert replay.status_code == 200
    assert replay.json() == first.json()


def test_secure_auth_rejects_plaintext_device_endpoints_by_default(client, monkeypatch):
    monkeypatch.setenv("NXPCTL_ALLOW_LEGACY_API", "0")
    assert client.post("/v1/devices/auth", json={
        "device_id": "switch-test", "device_credential": "x" * 40,
    }).status_code == 404
