from __future__ import annotations

import pytest

from server.secure_transport import (
    MessageContext, SecureMessageError, derive_key, open_json, open_message, seal,
)


def context(**overrides):
    values = dict(device_id="switch-test", method="POST", path="/v1/devices/switch-test/heartbeat", direction="request")
    values.update(overrides)
    return MessageContext(**values)


def test_aes_gcm_json_round_trip_and_context_binding():
    secret = b"enrollment-secret-material-32-bytes"
    key = derive_key(secret, "switch-test", "device")
    envelope = seal({"uptime_seconds": 123}, key, context(), nonce=b"n" * 12)
    assert open_json(envelope, key, context()) == {"uptime_seconds": 123}
    with pytest.raises(SecureMessageError):
        open_message(envelope, key, context(path="/v1/devices/switch-test/events:batch"))


def test_ciphertext_and_tag_tampering_are_rejected():
    key = derive_key(b"x" * 32, "switch-test")
    envelope = seal(b"secret", key, context(), nonce=b"z" * 12)
    changed = dict(envelope)
    changed["c"] = ("A" if envelope["c"][0] != "A" else "B") + envelope["c"][1:]
    with pytest.raises(SecureMessageError):
        open_message(changed, key, context())


def test_direction_method_device_and_path_are_authenticated():
    key = derive_key(b"x" * 32, "switch-test")
    envelope = seal(b"payload", key, context(), nonce=b"q" * 12)
    for altered in (
        context(direction="response"),
        context(method="GET"),
        context(device_id="other-device"),
        context(path="/v1/devices/switch-test/policy"),
    ):
        with pytest.raises(SecureMessageError):
            open_message(envelope, key, altered)


def test_nonce_and_key_domains_are_validated():
    with pytest.raises(SecureMessageError):
        seal(b"x", b"short", context(), nonce=b"n" * 12)
    with pytest.raises(SecureMessageError):
        seal(b"x", b"k" * 16, context(), nonce=b"short")
    assert derive_key(b"x" * 32, "switch-test", "enrollment") != derive_key(b"x" * 32, "switch-test", "device")


def test_envelope_schema_and_size_are_strict():
    key = derive_key(b"x" * 32, "switch-test")
    with pytest.raises(SecureMessageError):
        open_message({"v": 1, "n": "AA", "c": "AA", "extra": 1}, key, context())
    with pytest.raises(SecureMessageError):
        open_message({"v": 2, "n": "AA", "c": "AA"}, key, context())
    with pytest.raises(SecureMessageError):
        seal(b"x" * (64 * 1024 + 1), key, context())
