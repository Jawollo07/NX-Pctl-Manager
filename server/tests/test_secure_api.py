"""End-to-end tests for the encrypted device API using an isolated SQLite DB."""
from __future__ import annotations

import asyncio
import hashlib
import json
import os
import tempfile
import time
import unittest
from unittest.mock import patch

from starlette.requests import Request

from server import app as api
from server.secure_transport import MessageContext, derive_key_from_digest, open_json, seal_json


def make_request(path: str, envelope: dict[str, object]) -> Request:
    raw = json.dumps(envelope, separators=(",", ":")).encode("ascii")
    scope = {
        "type": "http",
        "asgi": {"version": "3.0"},
        "http_version": "1.1",
        "method": "POST",
        "scheme": "http",
        "path": path,
        "raw_path": path.encode("ascii"),
        "query_string": b"",
        "root_path": "",
        "headers": [(b"content-type", b"application/json"),
                    (b"content-length", str(len(raw)).encode("ascii"))],
        "server": ("testserver", 80),
        "client": ("127.0.0.1", 12345),
    }
    sent = False

    async def receive():
        nonlocal sent
        if sent:
            return {"type": "http.request", "body": b"", "more_body": False}
        sent = True
        return {"type": "http.request", "body": raw, "more_body": False}

    return Request(scope, receive)


class SecureDeviceApiIntegrationTests(unittest.TestCase):
    DEVICE_ID = "switch-integration"
    ENROLLMENT_CODE = "integration-enrollment-code-123456"
    PATH_PREFIX = f"/v1/secure/{DEVICE_ID}"

    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.database_path = os.path.join(self.tempdir.name, "isolated.sqlite3")
        self.env = patch.dict(os.environ, {"NXPCTL_DATABASE": self.database_path})
        self.env.start()
        api.init_database()
        self.enrollment_digest = hashlib.sha256(self.ENROLLMENT_CODE.encode()).digest()
        with api.database() as db:
            db.execute(
                "INSERT INTO enrollment_codes(code_hash,device_id,expires_at) VALUES(?,?,?)",
                (self.enrollment_digest.hex(), self.DEVICE_ID, int(time.time()) + 1800),
            )

    def tearDown(self):
        self.env.stop()
        self.tempdir.cleanup()

    def secure_call(self, operation: str, sequence: int, payload: dict[str, object],
                    secret_digest: bytes, purpose: str = "device"):
        path = f"{self.PATH_PREFIX}/{operation}"
        key = derive_key_from_digest(secret_digest, self.DEVICE_ID, purpose)
        context = MessageContext(self.DEVICE_ID, "POST", path, "request", sequence)
        envelope = seal_json(payload, key, context)
        envelope["k"] = secret_digest.hex()
        response = asyncio.run(
            api.secure_device_api(
                self.DEVICE_ID, operation, make_request(path, envelope)
            )
        )
        response_envelope = json.loads(response.body)
        response_context = MessageContext(self.DEVICE_ID, "POST", path, "response", sequence)
        decoded = open_json(
            {k: v for k, v in response_envelope.items() if k != "k"},
            key,
            response_context,
        )
        return response.status_code, decoded, response_envelope, envelope, key, path

    def test_enroll_authenticate_and_submit_encrypted_usage_heartbeat(self):
        status, enrolled, *_ = self.secure_call(
            "enroll", 1, {"enrollment_code": self.ENROLLMENT_CODE},
            self.enrollment_digest, "enrollment",
        )
        self.assertEqual(status, 200)
        self.assertEqual(enrolled["device_id"], self.DEVICE_ID)
        credential = enrolled["device_credential"]
        credential_digest = hashlib.sha256(credential.encode()).digest()

        status, authenticated, *_ = self.secure_call(
            "auth", 2, {"device_credential": credential}, credential_digest,
        )
        self.assertEqual(status, 200)
        self.assertEqual(authenticated["token_type"], "Bearer")
        token = authenticated["access_token"]

        heartbeat = {
            "access_token": token,
            "protocol_version": 1,
            "uptime_seconds": 90,
            "policy_revision": 0,
            "global_usage_day_key": 20758,
            "global_usage_active_seconds": 3600,
        }
        status, accepted, *_ = self.secure_call(
            "heartbeat", 3, heartbeat, credential_digest,
        )
        self.assertEqual(status, 200)
        self.assertTrue(accepted["accepted"])

        with api.database() as db:
            row = db.execute(
                "SELECT last_heartbeat_json FROM devices WHERE device_id=?",
                (self.DEVICE_ID,),
            ).fetchone()
        self.assertEqual(json.loads(row["last_heartbeat_json"]), {
            key: value for key, value in heartbeat.items() if key != "access_token"
        })

    def test_duplicate_sequence_returns_cached_encrypted_response(self):
        _, enrolled, *_ = self.secure_call(
            "enroll", 1, {"enrollment_code": self.ENROLLMENT_CODE},
            self.enrollment_digest, "enrollment",
        )
        credential = enrolled["device_credential"]
        credential_digest = hashlib.sha256(credential.encode()).digest()
        _, authenticated, *_ = self.secure_call(
            "auth", 2, {"device_credential": credential}, credential_digest,
        )
        token = authenticated["access_token"]

        original_payload = {
            "access_token": token,
            "protocol_version": 1,
            "uptime_seconds": 10,
        }
        first = self.secure_call("heartbeat", 77, original_payload, credential_digest)
        self.assertEqual(first[0], 200)

        # A different authenticated body reusing the same sequence must not
        # execute a second operation; the original encrypted response is replayed.
        altered_payload = {**original_payload, "uptime_seconds": 999}
        path = first[5]
        key = first[4]
        context = MessageContext(self.DEVICE_ID, "POST", path, "request", 77)
        altered_envelope = seal_json(altered_payload, key, context)
        altered_envelope["k"] = credential_digest.hex()
        replay = asyncio.run(
            api.secure_device_api(
                self.DEVICE_ID, "heartbeat", make_request(path, altered_envelope)
            )
        )
        self.assertEqual(replay.status_code, 200)
        self.assertEqual(json.loads(replay.body), first[2])

        with api.database() as db:
            row = db.execute(
                "SELECT last_heartbeat_json FROM devices WHERE device_id=?",
                (self.DEVICE_ID,),
            ).fetchone()
        self.assertEqual(json.loads(row["last_heartbeat_json"])["uptime_seconds"], 10)


    def _enroll_and_authenticate(self):
        _, enrolled, *_ = self.secure_call(
            "enroll", 1, {"enrollment_code": self.ENROLLMENT_CODE},
            self.enrollment_digest, "enrollment",
        )
        credential = enrolled["device_credential"]
        credential_digest = hashlib.sha256(credential.encode()).digest()
        _, authenticated, *_ = self.secure_call(
            "auth", 2, {"device_credential": credential}, credential_digest,
        )
        return credential_digest, authenticated["access_token"]

    def test_encrypted_policy_fetch_and_policy_ack(self):
        credential_digest, token = self._enroll_and_authenticate()
        now = int(time.time())
        policy = {
            "protocol_version": 1, "schema_version": 1,
            "device_id": self.DEVICE_ID, "revision": 1,
            "issued_at": now - 5, "not_after": now + 3600,
            "server_time": now - 5, "enabled": True,
            "require_parent_authentication": False, "offline_behavior": 0,
            "timezone_offset_minutes": 60, "global_daily_limit_minutes": 120,
            "weekday_limit_minutes": [65535] * 7,
            "quiet_hours_enabled": False, "quiet_hours_start_minute": 0,
            "quiet_hours_end_minute": 0, "title_rule_mode": 0, "title_rules": [],
        }
        with api.database() as db:
            db.execute(
                "INSERT INTO policies(device_id,revision,body_json,updated_at) VALUES(?,?,?,?)",
                (self.DEVICE_ID, 1, json.dumps(policy), now),
            )
        status, received, *_ = self.secure_call(
            "policy", 3, {"access_token": token}, credential_digest,
        )
        self.assertEqual(status, 200)
        self.assertEqual(received["revision"], 1)
        self.assertEqual(received["device_id"], self.DEVICE_ID)
        self.assertGreaterEqual(received["server_time"], now)

        ack = {"access_token": token, "revision": 1,
               "status": "accepted", "message": "validated"}
        status, result, *_ = self.secure_call(
            "policy-ack", 4, ack, credential_digest,
        )
        self.assertEqual(status, 200)
        self.assertTrue(result["accepted"])
        with api.database() as db:
            row = db.execute(
                "SELECT status,message FROM policy_acks WHERE device_id=? AND revision=1",
                (self.DEVICE_ID,),
            ).fetchone()
        self.assertEqual((row["status"], row["message"]), ("accepted", "validated"))

    def test_encrypted_events_batch_is_persisted_idempotently(self):
        credential_digest, token = self._enroll_and_authenticate()
        event = {
            "event_id": "evt-integration-1", "sequence": 1,
            "timestamp": int(time.time()), "type": "usage.started",
            "payload": {"source": "integration-test"},
        }
        payload = {"access_token": token, "events": [event]}
        status, result, *_ = self.secure_call("events", 3, payload, credential_digest)
        self.assertEqual(status, 200)
        self.assertEqual(result["accepted"], 1)
        self.assertEqual(result["received"], 1)

        status, duplicate, *_ = self.secure_call("events", 4, payload, credential_digest)
        self.assertEqual(status, 200)
        self.assertEqual(duplicate["accepted"], 0)
        self.assertEqual(duplicate["received"], 1)
        with api.database() as db:
            rows = db.execute(
                "SELECT event_id,event_type FROM events WHERE device_id=?",
                (self.DEVICE_ID,),
            ).fetchall()
        self.assertEqual([(row["event_id"], row["event_type"]) for row in rows],
                         [("evt-integration-1", "usage.started")])

if __name__ == "__main__":
    unittest.main()
