import importlib
import os
import sys
import tempfile
from pathlib import Path

from fastapi.testclient import TestClient

# Configure an isolated database before importing the application module.
_tmp = tempfile.TemporaryDirectory(prefix="nx-pctl-test-")
os.environ["CONTROL_DB_PATH"] = str(Path(_tmp.name) / "test.db")
os.environ["CONTROL_ADMIN_TOKEN"] = "test-admin-token-that-is-long-enough"
os.environ["CONTROL_ENROLLMENT_CODE"] = "test-one-time-enrollment-code-123456"
sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
api = importlib.import_module("app")


def test_enroll_policy_status_and_revoke():
    admin = {"Authorization": "Bearer test-admin-token-that-is-long-enough"}
    with TestClient(api.app) as client:
        enrolled = client.post("/api/v1/enroll", json={
            "enrollment_code": "test-one-time-enrollment-code-123456",
            "device_name": "Test Switch",
            "client_version": "test",
        })
        assert enrolled.status_code == 201
        data = enrolled.json()
        device_id, token = data["device_id"], data["device_token"]
        device_auth = {"Authorization": f"Bearer {token}"}

        # Enrollment is one-time; a second use must fail.
        again = client.post("/api/v1/enroll", json={
            "enrollment_code": "test-one-time-enrollment-code-123456",
            "device_name": "Second Switch",
        })
        assert again.status_code == 403

        checked = client.get("/api/v1/auth/check", headers=device_auth)
        assert checked.status_code == 200
        assert checked.json()["authenticated"] is True

        policy = client.get("/api/v1/policy", headers=device_auth)
        assert policy.status_code == 200
        assert policy.json()["policy"]["daily_limit_minutes"] is None

        changed = client.put(
            f"/api/v1/admin/devices/{device_id}/policy",
            headers=admin,
            json={"enabled": True, "daily_limit_minutes": 90, "note": "test"},
        )
        assert changed.status_code == 200
        assert changed.json()["version"] == 2

        status = client.post("/api/v1/status", headers=device_auth, json={
            "client_version": "test",
            "online": True,
            "policy_version": 2,
            "supported_features": ["daily_limit_minutes"],
            "applied_features": [],
            "warnings": [],
        })
        assert status.status_code == 204

        devices = client.get("/api/v1/admin/devices", headers=admin)
        assert devices.status_code == 200
        assert devices.json()[0]["device_id"] == device_id
        assert devices.json()[0]["last_status"]["online"] is True

        revoked = client.post(f"/api/v1/admin/devices/{device_id}/disable", headers=admin)
        assert revoked.status_code == 200
        assert client.get("/api/v1/policy", headers=device_auth).status_code == 401
