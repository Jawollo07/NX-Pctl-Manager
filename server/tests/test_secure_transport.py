"""Cross-language cryptographic fixtures shared with secure_crypto.cpp."""
from __future__ import annotations

import hashlib
import unittest

from server.secure_transport import (
    MessageContext,
    derive_key,
    derive_key_from_digest,
    open_json,
    seal_json,
)


class SecureTransportFixtureTests(unittest.TestCase):
    SECRET = b"enrollment-secret-material-32-bytes"
    DEVICE_ID = "switch-test"
    KEY = bytes.fromhex("6fda1bcef67c28a82b47cc840787a41b")
    NONCE = bytes.fromhex("000102030405060708090a0b")
    PLAINTEXT = {"protocol_version": 1, "uptime_seconds": 42}
    CIPHERTEXT_AND_TAG = bytes.fromhex(
        "d883079fd0fd8955162ab07db66c92224a147a2bccb1ee924a49ec619dc2a5e2"
        "2f2df2d7f5deaa7a529dcf0b9a99437e16687bf49a4b7a88ad4d"
    )

    def test_hkdf_matches_sysmodule_known_answer(self):
        self.assertEqual(
            derive_key(self.SECRET, self.DEVICE_ID, "device"),
            self.KEY,
        )
        self.assertEqual(
            derive_key_from_digest(
                hashlib.sha256(self.SECRET).digest(), self.DEVICE_ID, "device"
            ),
            self.KEY,
        )

    def test_aad_layout_matches_sysmodule_fixture(self):
        context = MessageContext(
            self.DEVICE_ID,
            "POST",
            "/v1/secure/switch-test/heartbeat",
            "request",
            123,
        )
        self.assertEqual(
            context.aad(),
            b"NX-PCTL-AEAD/1\nrequest\nPOST\n"
            b"/v1/secure/switch-test/heartbeat\nswitch-test\n123",
        )

    def test_aes_gcm_matches_sysmodule_known_answer(self):
        context = MessageContext(
            self.DEVICE_ID,
            "POST",
            "/v1/secure/switch-test/heartbeat",
            "request",
            123,
        )
        envelope = seal_json(self.PLAINTEXT, self.KEY, context, nonce=self.NONCE)
        self.assertEqual(envelope["n"], "AAECAwQFBgcICQoL")
        self.assertEqual(envelope["c"], (
            "2IMHn9D9iVUWKrB9tmySIkoUeivMse6SSknsYZ3CpeIvLfLX9d6qelKdzwuamUN-Fmh79JpLeoitTQ"
        ))
        self.assertEqual(
            __import__("base64").urlsafe_b64decode(envelope["c"] + "=" * (-len(envelope["c"]) % 4)),
            self.CIPHERTEXT_AND_TAG,
        )
        self.assertEqual(open_json(envelope, self.KEY, context), self.PLAINTEXT)

    def test_aad_context_change_rejects_ciphertext(self):
        context = MessageContext(
            self.DEVICE_ID,
            "POST",
            "/v1/secure/switch-test/heartbeat",
            "request",
            123,
        )
        envelope = seal_json(self.PLAINTEXT, self.KEY, context, nonce=self.NONCE)
        changed_context = MessageContext(
            self.DEVICE_ID,
            "POST",
            "/v1/secure/switch-test/policy",
            "request",
            123,
        )
        with self.assertRaises(ValueError):
            open_json(envelope, self.KEY, changed_context)


if __name__ == "__main__":
    unittest.main()
