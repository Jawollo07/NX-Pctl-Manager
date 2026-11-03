from __future__ import annotations

import json
import time

import pytest
from fastapi.testclient import TestClient

from server import app as api


@pytest.fixture()
def client(tmp_path, monkeypatch):
    monkeypatch.setenv("NXPCTL_DATABASE", str(tmp_path / "test.sqlite3"))
    api.init_database()
    with TestClient(api.app) as test_client:
        yield test_client


def enrollment_code(device_id="switch-test"):
    code = "enroll-" + ("x" * 32)
    with api.database() as db:
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
        "title_rule_mode": 0,
        "title_rules": [{"title_id": 72057594037932032, "action": 1}],
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
    event = {"event_id": "evt-1", "sequence": 1, "timestamp": int(time.time()), "type": "title.started", "payload": {"title_id": 123}}
    response = client.post("/v1/devices/switch-test/events:batch", headers=headers, json={"events": [event, event]})
    assert response.status_code == 200
    assert response.json()["accepted"] == 1
    assert response.json()["received"] == 2


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


def test_policy_validator_rejects_duplicate_title_ids():
    policy = valid_policy()
    policy["title_rules"].append(dict(policy["title_rules"][0]))
    with pytest.raises(Exception):
        api._validate_policy(policy, "switch-test")
