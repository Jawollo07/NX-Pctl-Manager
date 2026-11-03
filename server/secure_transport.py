"""Versioned end-to-end payload encryption for the NX-Pctl HTTP protocol.

HTTP is only a carrier. AES-128-GCM authenticates both ciphertext and the
request context (direction, method, path and device ID). This module is a
cryptographic building block; route integration and the matching Horizon
implementation are tracked separately.
"""
from __future__ import annotations

import base64
import hashlib
import json
import os
import re
from dataclasses import dataclass

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from cryptography.hazmat.primitives.kdf.hkdf import HKDF
from cryptography.hazmat.primitives import hashes

PROTOCOL_VERSION = 1
KEY_BYTES = 16
NONCE_BYTES = 12
TAG_BYTES = 16
MAX_PLAINTEXT_BYTES = 64 * 1024
_DEVICE_ID = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
_METHOD = re.compile(r"^(GET|POST)$")
_DIRECTION = re.compile(r"^(request|response)$")


class SecureMessageError(ValueError):
    """Invalid, unauthenticated, or oversized secure message."""


@dataclass(frozen=True)
class MessageContext:
    device_id: str
    method: str
    path: str
    direction: str
    sequence: int = 0

    def aad(self) -> bytes:
        if not _DEVICE_ID.fullmatch(self.device_id):
            raise SecureMessageError("invalid device id")
        if not _METHOD.fullmatch(self.method):
            raise SecureMessageError("unsupported HTTP method")
        if not self.path.startswith("/") or len(self.path) > 512 or any(ord(c) < 0x21 or ord(c) > 0x7e for c in self.path):
            raise SecureMessageError("invalid request path")
        if not _DIRECTION.fullmatch(self.direction):
            raise SecureMessageError("invalid message direction")
        if type(self.sequence) is not int or not 0 <= self.sequence <= 0xFFFFFFFFFFFFFFFF:
            raise SecureMessageError("invalid message sequence")
        return (
            "NX-PCTL-AEAD/1\n"
            f"{self.direction}\n{self.method}\n{self.path}\n{self.device_id}\n{self.sequence}"
        ).encode("ascii")


def derive_key(secret: bytes, device_id: str, purpose: str = "device") -> bytes:
    """Derive a key from raw secret bytes using SHA-256 then HKDF-SHA-256."""
    if not isinstance(secret, bytes) or len(secret) < 16 or len(secret) > 128:
        raise SecureMessageError("secret must contain 16..128 bytes")
    return derive_key_from_digest(hashlib.sha256(secret).digest(), device_id, purpose)


def derive_key_from_digest(secret_digest: bytes, device_id: str, purpose: str = "device") -> bytes:
    """Derive the same key from a stored SHA-256 secret hash (server-side)."""
    if not isinstance(secret_digest, bytes) or len(secret_digest) != 32:
        raise SecureMessageError("32-byte SHA-256 digest required")
    if not _DEVICE_ID.fullmatch(device_id):
        raise SecureMessageError("invalid device id")
    if purpose not in ("enrollment", "device"):
        raise SecureMessageError("invalid key purpose")
    info = f"NX-PCTL-AEAD/1/{purpose}/{device_id}".encode("ascii")
    return HKDF(
        algorithm=hashes.SHA256(),
        length=KEY_BYTES,
        salt=b"NX-PCTL-AEAD-1",
        info=info,
    ).derive(secret_digest)

def _b64encode(value: bytes) -> str:
    return base64.urlsafe_b64encode(value).rstrip(b"=").decode("ascii")


def _b64decode(value: object, *, maximum: int) -> bytes:
    if not isinstance(value, str) or not value or len(value) > maximum:
        raise SecureMessageError("invalid envelope encoding")
    if not re.fullmatch(r"[A-Za-z0-9_-]+", value):
        raise SecureMessageError("non-canonical base64url")
    try:
        decoded = base64.urlsafe_b64decode(value + "=" * (-len(value) % 4))
    except Exception as exc:
        raise SecureMessageError("invalid base64url") from exc
    if _b64encode(decoded) != value:
        raise SecureMessageError("non-canonical base64url")
    return decoded


def seal(plaintext: bytes, key: bytes, context: MessageContext, *, nonce: bytes | None = None) -> dict[str, object]:
    if not isinstance(plaintext, bytes) or len(plaintext) > MAX_PLAINTEXT_BYTES:
        raise SecureMessageError("plaintext is oversized")
    if not isinstance(key, bytes) or len(key) != KEY_BYTES:
        raise SecureMessageError("AES-128 key required")
    nonce = os.urandom(NONCE_BYTES) if nonce is None else nonce
    if not isinstance(nonce, bytes) or len(nonce) != NONCE_BYTES:
        raise SecureMessageError("96-bit nonce required")
    ciphertext = AESGCM(key).encrypt(nonce, plaintext, context.aad())
    return {"v": PROTOCOL_VERSION, "s": context.sequence, "n": _b64encode(nonce), "c": _b64encode(ciphertext)}


def open_message(envelope: object, key: bytes, context: MessageContext) -> bytes:
    if not isinstance(envelope, dict) or set(envelope) != {"v", "s", "n", "c"}:
        raise SecureMessageError("invalid envelope fields")
    if type(envelope["v"]) is not int or envelope["v"] != PROTOCOL_VERSION:
        raise SecureMessageError("unsupported secure-message version")
    if type(envelope["s"]) is not int or envelope["s"] != context.sequence:
        raise SecureMessageError("message sequence mismatch")
    if not isinstance(key, bytes) or len(key) != KEY_BYTES:
        raise SecureMessageError("AES-128 key required")
    nonce = _b64decode(envelope["n"], maximum=32)
    ciphertext = _b64decode(envelope["c"], maximum=((MAX_PLAINTEXT_BYTES + TAG_BYTES + 2) * 4 // 3) + 8)
    if len(nonce) != NONCE_BYTES or not TAG_BYTES <= len(ciphertext) <= MAX_PLAINTEXT_BYTES + TAG_BYTES:
        raise SecureMessageError("invalid encrypted message size")
    try:
        return AESGCM(key).decrypt(nonce, ciphertext, context.aad())
    except Exception as exc:
        raise SecureMessageError("message authentication failed") from exc


def seal_json(value: object, key: bytes, context: MessageContext) -> dict[str, object]:
    raw = json.dumps(value, separators=(",", ":"), sort_keys=True, ensure_ascii=True).encode("ascii")
    return seal(raw, key, context)


def _unique_json_object(pairs: list[tuple[str, object]]) -> dict[str, object]:
    result: dict[str, object] = {}
    for key, value in pairs:
        if key in result:
            raise SecureMessageError("duplicate decrypted JSON field")
        result[key] = value
    return result


def _reject_json_number(_: str) -> object:
    raise SecureMessageError("non-integer JSON numbers are not accepted")


def open_json(envelope: object, key: bytes, context: MessageContext) -> object:
    plaintext = open_message(envelope, key, context)
    try:
        return json.loads(plaintext.decode("ascii"), object_pairs_hook=_unique_json_object, parse_float=_reject_json_number, parse_constant=_reject_json_number)
    except (UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise SecureMessageError("decrypted payload is not valid JSON") from exc
