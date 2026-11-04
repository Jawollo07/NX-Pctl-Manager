"""Regression tests for the global daily usage heartbeat and CLI reporting."""
from __future__ import annotations

import contextlib
import io
import json
import os
import tempfile
import unittest
from unittest.mock import patch

from fastapi import HTTPException

from server.app import _validate_heartbeat_body, database, init_database
from server.cli import list_devices


class HeartbeatValidationTests(unittest.TestCase):
    def test_accepts_heartbeat_without_usage_snapshot(self):
        self.assertEqual(
            _validate_heartbeat_body({"protocol_version": 1, "uptime_seconds": 12}),
            {"protocol_version": 1, "uptime_seconds": 12},
        )

    def test_accepts_valid_global_usage_snapshot(self):
        body = {
            "protocol_version": 1,
            "global_usage_day_key": 20758,
            "global_usage_active_seconds": 3600,
        }
        self.assertEqual(_validate_heartbeat_body(body), body)

    def test_requires_day_and_seconds_together(self):
        for body in (
            {"global_usage_day_key": 20758},
            {"global_usage_active_seconds": 3600},
        ):
            with self.subTest(body=body), self.assertRaises(HTTPException) as caught:
                _validate_heartbeat_body(body)
            self.assertEqual(caught.exception.status_code, 422)

    def test_rejects_boolean_as_integer(self):
        with self.assertRaises(HTTPException):
            _validate_heartbeat_body({
                "global_usage_day_key": True,
                "global_usage_active_seconds": 1,
            })

    def test_rejects_usage_over_one_day(self):
        with self.assertRaises(HTTPException) as caught:
            _validate_heartbeat_body({
                "global_usage_day_key": 20758,
                "global_usage_active_seconds": 86401,
            })
        self.assertEqual(caught.exception.status_code, 422)

    def test_rejects_day_key_outside_signed_64_bit_range(self):
        with self.assertRaises(HTTPException) as caught:
            _validate_heartbeat_body({
                "global_usage_day_key": 0x8000000000000000,
                "global_usage_active_seconds": 1,
            })
        self.assertEqual(caught.exception.status_code, 422)

    def test_rejects_negative_and_unsupported_protocol_values(self):
        with self.assertRaises(HTTPException):
            _validate_heartbeat_body({"uptime_seconds": -1})
        with self.assertRaises(HTTPException):
            _validate_heartbeat_body({"protocol_version": 2})


class DeviceListingTests(unittest.TestCase):
    def test_list_devices_decodes_latest_heartbeat_json(self):
        with tempfile.TemporaryDirectory() as directory:
            database_path = os.path.join(directory, "test.sqlite3")
            heartbeat = {
                "protocol_version": 1,
                "global_usage_day_key": 20758,
                "global_usage_active_seconds": 3600,
            }
            with patch.dict(os.environ, {"NXPCTL_DATABASE": database_path}):
                init_database()
                with database() as db:
                    db.execute(
                        "INSERT INTO devices(device_id,credential_hash,created_at,last_heartbeat_json) "
                        "VALUES(?,?,?,?)",
                        ("switch-test", "not-a-real-credential-hash", 1,
                         json.dumps(heartbeat)),
                    )
                output = io.StringIO()
                with contextlib.redirect_stdout(output):
                    result = list_devices(None)

            self.assertEqual(result, 0)
            row = json.loads(output.getvalue().strip())
            self.assertEqual(row["device_id"], "switch-test")
            self.assertEqual(row["last_heartbeat"], heartbeat)
            self.assertNotIn("last_heartbeat_json", row)


if __name__ == "__main__":
    unittest.main()
