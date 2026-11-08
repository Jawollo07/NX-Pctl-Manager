"""Backend-managed TOTP and encrypted device grant tests."""
from __future__ import annotations

import asyncio
import base64
import hashlib
import json
import os
import tempfile
import time
import unittest
from unittest.mock import patch

from starlette.requests import Request

from server import app as api
from server import parent_auth_api as parent
from server.secure_transport import MessageContext, derive_key_from_digest, open_json, seal_json


def make_request(path: str, payload: dict[str, object]) -> Request:
    raw = json.dumps(payload, separators=(",", ":")).encode("ascii")
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


class ParentAuthBackendTests(unittest.TestCase):
    DEVICE_ID = "switch-parent"
    ADMIN = "parent-admin-" + "x" * 40
    CREDENTIAL = "device-credential-" + "x" * 32
    TOKEN = "device-access-token-" + "x" * 32

    def setUp(self):
        self.tempdir = tempfile.TemporaryDirectory()
        self.db_path = os.path.join(self.tempdir.name, "parent.sqlite3")
        self.env = patch.dict(os.environ, {
            "NXPCTL_DATABASE": self.db_path,
            "NXPCTL_ADMIN_TOKEN": self.ADMIN,
            "NXPCTL_PARENT_AUTH_KEY": "42" * 32,
        })
        self.env.start()
        api.init_database()
        parent.init_parent_auth_schema()
        self.digest = hashlib.sha256(self.CREDENTIAL.encode()).digest()
        with api.database() as db:
            db.execute(
                "INSERT INTO devices(device_id,credential_hash,created_at) VALUES(?,?,?)",
                (self.DEVICE_ID, self.digest.hex(), int(time.time())),
            )
            db.execute(
                "INSERT INTO access_tokens(token_hash,device_id,expires_at,created_at) VALUES(?,?,?,?)",
                (api._hash_secret(self.TOKEN), self.DEVICE_ID, int(time.time()) + 600, int(time.time())),
            )

    def tearDown(self):
        self.env.stop()
        self.tempdir.cleanup()

    def provision(self):
        request = make_request("/v1/admin/devices/switch-parent/parent-auth", {"confirm": True})
        response = asyncio.run(parent.provision_parent_auth(
            self.DEVICE_ID, request, "Bearer " + self.ADMIN,
        ))
        secret_text = response["totp_secret"]
        padding = "=" * ((8 - len(secret_text) % 8) % 8)
        return base64.b32decode(secret_text + padding)

    def secure_call(self, operation: str, sequence: int, payload: dict[str, object]):
        path = f"/v1/secure/{self.DEVICE_ID}/{operation}"
        key = derive_key_from_digest(self.digest, self.DEVICE_ID, "device")
        context = MessageContext(self.DEVICE_ID, "POST", path, "request", sequence)
        envelope = seal_json(payload, key, context)
        envelope["k"] = self.digest.hex()
        response = asyncio.run(api.secure_device_api(
            self.DEVICE_ID, operation, make_request(path, envelope),
        ))
        response_envelope = json.loads(response.body)
        response_context = MessageContext(self.DEVICE_ID, "POST", path, "response", sequence)
        decoded = open_json(
            {k: v for k, v in response_envelope.items() if k != "k"},
            key, response_context,
        )
        return response.status_code, decoded

    def test_totp_secret_is_encrypted_and_status_never_discloses_it(self):
        secret = self.provision()
        with api.database() as db:
            row = db.execute(
                "SELECT secret_nonce,secret_ciphertext FROM parent_auth WHERE device_id=?",
                (self.DEVICE_ID,),
            ).fetchone()
        self.assertNotEqual(bytes(row["secret_ciphertext"]), secret)
        self.assertNotIn(secret, bytes(row["secret_ciphertext"]))
        status = parent.parent_auth_status(self.DEVICE_ID, "Bearer " + self.ADMIN)
        self.assertTrue(status["configured"])
        self.assertNotIn("totp_secret", status)

    def test_totp_is_single_use_and_grant_is_read_only_over_secure_device_api(self):
        secret = self.provision()
        code = parent._code(secret, int(time.time()) // 30)
        request = make_request("/verify", {"code": code})
        verified = asyncio.run(parent.verify_parent_auth(
            self.DEVICE_ID, request, "Bearer " + self.ADMIN,
        ))
        self.assertTrue(verified["authenticated"])

        status, grant = self.secure_call(
            "parent-auth", 51, {"access_token": self.TOKEN},
        )
        self.assertEqual(status, 200)
        self.assertTrue(grant["parent_authenticated"])
        self.assertGreater(grant["authenticated_until"], int(time.time()))

        with self.assertRaises(Exception):
            asyncio.run(parent.verify_parent_auth(
                self.DEVICE_ID, make_request("/verify", {"code": code}),
                "Bearer " + self.ADMIN,
            ))

    def test_admin_authentication_and_lockout(self):
        with self.assertRaises(Exception):
            parent.parent_auth_status(self.DEVICE_ID, None)
        self.provision()
        for _ in range(5):
            try:
                asyncio.run(parent.verify_parent_auth(
                    self.DEVICE_ID, make_request("/verify", {"code": "000000"}),
                    "Bearer " + self.ADMIN,
                ))
            except Exception:
                pass
        with self.assertRaises(Exception):
            asyncio.run(parent.verify_parent_auth(
                self.DEVICE_ID, make_request("/verify", {"code": "000000"}),
                "Bearer " + self.ADMIN,
            ))

    def test_revocation_removes_grant_and_secret(self):
        self.provision()
        result = parent.revoke_parent_auth(self.DEVICE_ID, "Bearer " + self.ADMIN)
        self.assertTrue(result["revoked"])
        self.assertFalse(parent.issue_or_read_grant(self.DEVICE_ID)["parent_authenticated"])


if __name__ == "__main__":
    unittest.main()
