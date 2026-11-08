"""Backend-owned parent authentication.

TOTP secrets never leave the backend. The administrator verifies a code here;
the Switch can only read the resulting short-lived authorization state through
its existing encrypted device channel.
"""
from __future__ import annotations

import base64
import hashlib
import hmac
import os
import re
import secrets
import struct
import time
from typing import Any

from cryptography.hazmat.primitives.ciphers.aead import AESGCM
from fastapi import APIRouter, Header, HTTPException, Request

router = APIRouter()
SECRET_BYTES = 20
SESSION_SECONDS = 15 * 60
LOCKOUT_SECONDS = 5 * 60
MAX_FAILURES = 5
DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")


def _api():
    try:
        from . import app as api
    except ImportError:  # pragma: no cover
        import app as api
    return api


def _master_key() -> bytes:
    value = os.environ.get("NXPCTL_PARENT_AUTH_KEY", "")
    if not re.fullmatch(r"[0-9a-fA-F]{64}", value):
        raise HTTPException(503, "parent authentication encryption key is not configured")
    return bytes.fromhex(value)


def _admin(authorization: str | None) -> None:
    expected = os.environ.get("NXPCTL_ADMIN_TOKEN", "")
    if len(expected) < 32:
        raise HTTPException(503, "backend administrator token is not configured")
    scheme, _, supplied = (authorization or "").partition(" ")
    if scheme.lower() != "bearer" or not hmac.compare_digest(supplied, expected):
        raise HTTPException(401, "backend administrator authentication required")


def _encrypt_secret(device_id: str, secret: bytes) -> tuple[bytes, bytes]:
    nonce = secrets.token_bytes(12)
    ciphertext = AESGCM(_master_key()).encrypt(nonce, secret, device_id.encode("ascii"))
    return nonce, ciphertext


def _decrypt_secret(device_id: str, nonce: bytes, ciphertext: bytes) -> bytes:
    try:
        return AESGCM(_master_key()).decrypt(nonce, ciphertext, device_id.encode("ascii"))
    except Exception as exc:
        raise HTTPException(503, "parent authentication secret cannot be decrypted") from exc


def _code(secret: bytes, counter: int) -> str:
    digest = hmac.new(secret, struct.pack(">Q", counter), hashlib.sha1).digest()
    offset = digest[-1] & 0x0F
    value = struct.unpack(">I", digest[offset:offset + 4])[0] & 0x7FFFFFFF
    return f"{value % 1_000_000:06d}"


def _matching_counter(secret: bytes, supplied: Any, now: int, last_counter: int) -> int | None:
    if not isinstance(supplied, str) or not re.fullmatch(r"[0-9]{6}", supplied):
        return None
    current = now // 30
    match = None
    for counter in range(max(0, current - 1), current + 2):
        if counter > last_counter and hmac.compare_digest(_code(secret, counter), supplied):
            match = counter
    return match


def init_parent_auth_schema() -> None:
    api = _api()
    with api.database() as db:
        db.execute("""
            CREATE TABLE IF NOT EXISTS parent_auth (
                device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
                secret_nonce BLOB NOT NULL,
                secret_ciphertext BLOB NOT NULL,
                last_counter INTEGER NOT NULL DEFAULT -1,
                failed_attempts INTEGER NOT NULL DEFAULT 0,
                locked_until INTEGER NOT NULL DEFAULT 0,
                grant_expires_at INTEGER NOT NULL DEFAULT 0,
                created_at INTEGER NOT NULL,
                updated_at INTEGER NOT NULL
            )
        """)


def issue_or_read_grant(device_id: str) -> dict[str, Any]:
    """Called only after the secure device envelope has been authenticated."""
    api = _api()
    now = int(time.time())
    with api.database() as db:
        row = db.execute(
            """SELECT grant_expires_at FROM parent_auth
               WHERE device_id=?""", (device_id,)
        ).fetchone()
    expires = int(row["grant_expires_at"]) if row else 0
    active = expires > now
    return {"parent_authenticated": active, "authenticated_until": expires if active else 0,
            "server_time": now}


@router.post("/v1/admin/devices/{device_id}/parent-auth")
async def provision_parent_auth(
    device_id: str, request: Request, authorization: str | None = Header(default=None)
):
    _admin(authorization)
    if not DEVICE_ID_RE.fullmatch(device_id):
        raise HTTPException(404, "unknown device")
    body = await request.json()
    if body != {"confirm": True}:
        raise HTTPException(422, "explicit confirmation required: confirm=true")
    api = _api()
    with api.database() as db:
        device = db.execute(
            "SELECT 1 FROM devices WHERE device_id=? AND revoked_at IS NULL", (device_id,)
        ).fetchone()
        if device is None:
            raise HTTPException(404, "unknown or revoked device")
        secret = secrets.token_bytes(SECRET_BYTES)
        nonce, ciphertext = _encrypt_secret(device_id, secret)
        now = int(time.time())
        db.execute(
            """INSERT INTO parent_auth(
                   device_id,secret_nonce,secret_ciphertext,last_counter,
                   failed_attempts,locked_until,grant_expires_at,created_at,updated_at
               ) VALUES(?,?,?,-1,0,0,0,?,?)
               ON CONFLICT(device_id) DO UPDATE SET
                   secret_nonce=excluded.secret_nonce,
                   secret_ciphertext=excluded.secret_ciphertext,
                   last_counter=-1,failed_attempts=0,locked_until=0,
                   grant_expires_at=0,updated_at=excluded.updated_at""",
            (device_id, nonce, ciphertext, now, now),
        )
    encoded = base64.b32encode(secret).decode("ascii").rstrip("=")
    return {"device_id": device_id, "configured": True, "totp_secret": encoded,
            "algorithm": "SHA1", "digits": 6, "period_seconds": 30,
            "session_seconds": SESSION_SECONDS, "secret_displayed_once": True}


@router.get("/v1/admin/devices/{device_id}/parent-auth")
def parent_auth_status(
    device_id: str, authorization: str | None = Header(default=None)
):
    _admin(authorization)
    api = _api()
    with api.database() as db:
        row = db.execute(
            """SELECT failed_attempts,locked_until,grant_expires_at,updated_at
               FROM parent_auth WHERE device_id=?""", (device_id,)
        ).fetchone()
    if row is None:
        return {"device_id": device_id, "configured": False, "authenticated": False}
    now = int(time.time())
    return {
        "device_id": device_id, "configured": True,
        "authenticated": int(row["grant_expires_at"]) > now,
        "authenticated_until": int(row["grant_expires_at"]),
        "failed_attempts": int(row["failed_attempts"]),
        "locked_until": int(row["locked_until"]),
        "updated_at": int(row["updated_at"]),
    }


@router.post("/v1/admin/devices/{device_id}/parent-auth/verify")
async def verify_parent_auth(
    device_id: str, request: Request, authorization: str | None = Header(default=None)
):
    _admin(authorization)
    body = await request.json()
    if not isinstance(body, dict) or set(body) != {"code"}:
        raise HTTPException(422, "body must contain only code")
    api = _api()
    now = int(time.time())
    with api.database() as db:
        db.execute("BEGIN IMMEDIATE")
        row = db.execute(
            """SELECT secret_nonce,secret_ciphertext,last_counter,failed_attempts,
                      locked_until FROM parent_auth WHERE device_id=?""", (device_id,)
        ).fetchone()
        if row is None:
            db.rollback()
            raise HTTPException(404, "parent authentication is not configured")
        if int(row["locked_until"]) > now:
            db.rollback()
            raise HTTPException(429, "parent authentication is temporarily locked")
        failed_attempts = 0 if int(row["locked_until"]) != 0 else int(row["failed_attempts"])
        secret = _decrypt_secret(device_id, bytes(row["secret_nonce"]), bytes(row["secret_ciphertext"]))
        matched = _matching_counter(secret, body["code"], now, int(row["last_counter"]))
        # Best-effort wipe of the mutable copy; immutable bytes are short-lived.
        if matched is None:
            failures = failed_attempts + 1
            locked_until = now + LOCKOUT_SECONDS if failures >= MAX_FAILURES else 0
            db.execute(
                """UPDATE parent_auth SET failed_attempts=?,locked_until=?,
                   grant_expires_at=0,updated_at=? WHERE device_id=?""",
                (min(failures, MAX_FAILURES), locked_until, now, device_id),
            )
            db.commit()
            raise HTTPException(429 if locked_until else 401,
                                "parent authentication locked" if locked_until else "invalid or replayed TOTP code")
        expires = now + SESSION_SECONDS
        db.execute(
            """UPDATE parent_auth SET last_counter=?,failed_attempts=0,locked_until=0,
               grant_expires_at=?,updated_at=? WHERE device_id=?""",
            (matched, expires, now, device_id),
        )
        db.commit()
    return {"device_id": device_id, "authenticated": True,
            "authenticated_until": expires, "session_seconds": SESSION_SECONDS}


@router.delete("/v1/admin/devices/{device_id}/parent-auth")
def revoke_parent_auth(
    device_id: str, authorization: str | None = Header(default=None)
):
    _admin(authorization)
    api = _api()
    with api.database() as db:
        cursor = db.execute("DELETE FROM parent_auth WHERE device_id=?", (device_id,))
    return {"device_id": device_id, "revoked": cursor.rowcount == 1}
