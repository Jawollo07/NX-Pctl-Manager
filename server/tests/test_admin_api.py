"""Tests for the backend-only administration control plane."""
from __future__ import annotations

import asyncio
import json
import os
import tempfile
import time
import unittest
from unittest.mock import patch

from starlette.requests import Request

from server import app as api
from server import admin_api


def make_request(path: str, body: dict[str, object]) -> Request:
    raw = json.dumps(body, separators=(",", ":")).encode("ascii")
    scope = {
        "type": "http", "asgi": {"version": "3.0"}, "http_version": "1.1",
        "method": "POST", "scheme": "http", "path": path,
        "raw_path": path.encode("ascii"), "query_string": b"", "root_path": "",
        "headers": [(b"content-type", b"application/json"),
                    (b"content-length", str(len(raw)).encode("ascii"))],
        "server": ("testserver", 80), "client": ("127.0.0.1", 12345),
    }
    sent = False

    async def receive():
        nonlocal sent
        if sent:
            return {"type": "http.request", "body": b"", "more_body": False}
        sent = True
        return {"type": "http.request", "body": raw, "more_body": False}

    return Request(scope, receive)


class AdminControlPlaneTests(unittest.TestCase):
    ADMIN = "admin-control-plane-" + "x" * 40
    DEVICE_ID = "switch-admin"

    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.env = patch.dict(os.environ, {
            "NXPCTL_DATABASE": os.path.join(self.tempdir.name, "admin.sqlite3"),
            "NXPCTL_ADMIN_TOKEN": self.ADMIN,
            "NXPCTL_PARENT_AUTH_KEY": "43" * 32,
        })
        self.env.start()
        api.init_database()

    def tearDown(self):
        self.env.stop()
        self.tempdir.cleanup()

    def admin(self):
        return "Bearer " + self.ADMIN

    def enroll_device(self):
        result = asyncio.run(admin_api.create_enrollment(
            self.DEVICE_ID,
            make_request("/enrollment", {"ttl_seconds": 900}),
            self.admin(),
        ))
        self.assertEqual(result["device_id"], self.DEVICE_ID)
        self.assertEqual(result["displayed_once"], True)
        with api.database() as db:
            db.execute(
                "INSERT INTO devices(device_id,credential_hash,created_at) VALUES(?,?,?)",
                (self.DEVICE_ID, api._hash_secret("credential"), int(time.time())),
            )
        return result["enrollment_code"]

    def policy(self, revision=1):
        now = int(time.time())
        return {
            "protocol_version": 1, "schema_version": 1,
            "device_id": self.DEVICE_ID, "revision": revision,
            "issued_at": now, "not_after": now + 3600, "server_time": now,
            "enabled": True, "require_parent_authentication": False,
            "offline_behavior": 0, "timezone_offset_minutes": 60,
            "global_daily_limit_minutes": 90,
            "weekday_limit_minutes": [65535] * 7,
            "quiet_hours_enabled": False,
            "quiet_hours_start_minute": 1320, "quiet_hours_end_minute": 420,
        }

    def test_policy_is_published_and_revision_is_monotonic(self):
        self.enroll_device()
        first = asyncio.run(admin_api.publish_policy(
            self.DEVICE_ID,
            make_request("/policy", self.policy(1)),
            self.admin(),
        ))
        self.assertEqual(first["revision"], 1)
        with self.assertRaises(Exception):
            asyncio.run(admin_api.publish_policy(
                self.DEVICE_ID,
                make_request("/policy", self.policy(1)),
                self.admin(),
            ))
        stored = admin_api.read_policy(self.DEVICE_ID, self.admin())
        self.assertEqual(stored["policy"]["global_daily_limit_minutes"], 90)

    def test_device_inventory_and_revocation_are_backend_controlled(self):
        self.enroll_device()
        inventory = admin_api.list_devices(self.admin())
        self.assertEqual(inventory["devices"][0]["device_id"], self.DEVICE_ID)
        revoked = admin_api.revoke_device(self.DEVICE_ID, self.admin())
        self.assertTrue(revoked["revoked"])
        with api.database() as db:
            row = db.execute(
                "SELECT revoked_at FROM devices WHERE device_id=?", (self.DEVICE_ID,)
            ).fetchone()
        self.assertIsNotNone(row["revoked_at"])

    def test_admin_routes_reject_missing_or_invalid_admin_token(self):
        with self.assertRaises(Exception):
            admin_api.list_devices(None)
        with self.assertRaises(Exception):
            admin_api.list_devices("Bearer invalid")

    def test_enrollment_rejects_active_device_and_invalid_ttl(self):
        self.enroll_device()
        with self.assertRaises(Exception):
            asyncio.run(admin_api.create_enrollment(
                self.DEVICE_ID, make_request("/enrollment", {"ttl_seconds": 900}),
                self.admin(),
            ))
        with self.assertRaises(Exception):
            asyncio.run(admin_api.create_enrollment(
                "other-device", make_request("/enrollment", {"ttl_seconds": 5}),
                self.admin(),
            ))


if __name__ == "__main__":
    unittest.main()
