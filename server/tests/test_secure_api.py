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


if __name__ == "__main__":
    unittest.main()
