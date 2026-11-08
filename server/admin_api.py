"""Authenticated backend administration API.

The API is the control plane for enrollment, device lifecycle, global policy,
usage/event inspection and parent-auth configuration. The Switch never accepts
policy edits or parent-auth setup through local HIPC commands.
"""
from __future__ import annotations

import json
import re
import secrets
import time
from typing import Any

from fastapi import APIRouter, Header, HTTPException, Request

router = APIRouter()
DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")


def _api():
    try:
        from . import app as api
    except ImportError:  # pragma: no cover
        import app as api
    return api


def _admin(authorization: str | None) -> None:
    try:
        from .parent_auth_api import _admin as authenticate_admin
    except ImportError:  # pragma: no cover
        from parent_auth_api import _admin as authenticate_admin
    authenticate_admin(authorization)


def _device_id(value: str) -> None:
    if not DEVICE_ID_RE.fullmatch(value):
        raise HTTPException(404, "unknown device")


@router.get("/v1/admin/devices")
def list_devices(authorization: str | None = Header(default=None)):
    _admin(authorization)
    api = _api()
    with api.database() as db:
        rows = db.execute(
            """SELECT d.device_id,d.created_at,d.revoked_at,d.last_auth_at,
                      d.last_seen_at,d.last_heartbeat_json,p.revision AS policy_revision,
                      CASE WHEN pa.device_id IS NULL THEN 0 ELSE 1 END AS parent_auth_configured,
                      pa.grant_expires_at
               FROM devices d
               LEFT JOIN policies p USING(device_id)
               LEFT JOIN parent_auth pa USING(device_id)
               ORDER BY d.device_id"""
        ).fetchall()
    now = int(time.time())
    result = []
    for row in rows:
        heartbeat = json.loads(row["last_heartbeat_json"]) if row["last_heartbeat_json"] else None
        result.append({
            "device_id": row["device_id"], "created_at": row["created_at"],
            "revoked_at": row["revoked_at"], "last_auth_at": row["last_auth_at"],
            "last_seen_at": row["last_seen_at"], "last_heartbeat": heartbeat,
            "policy_revision": row["policy_revision"],
            "parent_auth_configured": bool(row["parent_auth_configured"]),
            "parent_authenticated": int(row["grant_expires_at"] or 0) > now,
        })
    return {"devices": result}


@router.post("/v1/admin/devices/{device_id}/enrollment")
async def create_enrollment(
    device_id: str, request: Request, authorization: str | None = Header(default=None)
):
    _admin(authorization)
    _device_id(device_id)
    body = await request.json()
    ttl = body.get("ttl_seconds", 1800) if isinstance(body, dict) else None
    if not isinstance(ttl, int) or isinstance(ttl, bool) or not 60 <= ttl <= 86400:
        raise HTTPException(422, "ttl_seconds must be between 60 and 86400")
    api = _api()
    now = int(time.time())
    code = secrets.token_urlsafe(24)
    with api.database() as db:
        existing = db.execute(
            "SELECT revoked_at FROM devices WHERE device_id=?", (device_id,)
        ).fetchone()
        if existing is not None and existing["revoked_at"] is None:
            raise HTTPException(409, "device is already enrolled")
        db.execute("DELETE FROM enrollment_codes WHERE device_id=?", (device_id,))
        db.execute(
            "INSERT INTO enrollment_codes(code_hash,device_id,expires_at) VALUES(?,?,?)",
            (api._hash_secret(code), device_id, now + ttl),
        )
    return {"device_id": device_id, "enrollment_code": code,
            "expires_at": now + ttl, "displayed_once": True}


@router.get("/v1/admin/devices/{device_id}/policy")
def read_policy(device_id: str, authorization: str | None = Header(default=None)):
    _admin(authorization)
    _device_id(device_id)
    api = _api()
    with api.database() as db:
        row = db.execute(
            "SELECT revision,body_json,updated_at FROM policies WHERE device_id=?",
            (device_id,),
        ).fetchone()
    if row is None:
        raise HTTPException(404, "no policy configured")
    return {"revision": int(row["revision"]), "updated_at": int(row["updated_at"]),
            "policy": json.loads(row["body_json"])}


@router.put("/v1/admin/devices/{device_id}/policy")
async def publish_policy(
    device_id: str, request: Request, authorization: str | None = Header(default=None)
):
    _admin(authorization)
    _device_id(device_id)
    api = _api()
    policy = await request.json()
    if not isinstance(policy, dict):
        raise HTTPException(400, "policy JSON object required")
    policy["device_id"] = device_id
    policy["server_time"] = int(time.time())
    api._validate_policy(policy, device_id)
    serialized = json.dumps(policy, separators=(",", ":"), ensure_ascii=True)
    now = int(time.time())
    with api.database() as db:
        db.execute("BEGIN IMMEDIATE")
        device = db.execute(
            "SELECT 1 FROM devices WHERE device_id=? AND revoked_at IS NULL",
            (device_id,),
        ).fetchone()
        if device is None:
            db.rollback()
            raise HTTPException(404, "unknown or revoked device")
        current = db.execute(
            "SELECT revision FROM policies WHERE device_id=?", (device_id,)
        ).fetchone()
        if current is not None and policy["revision"] <= int(current["revision"]):
            db.rollback()
            raise HTTPException(409, "policy revision must increase monotonically")
        db.execute(
            """INSERT INTO policies(device_id,revision,body_json,updated_at)
               VALUES(?,?,?,?) ON CONFLICT(device_id) DO UPDATE SET
               revision=excluded.revision,body_json=excluded.body_json,
               updated_at=excluded.updated_at""",
            (device_id, policy["revision"], serialized, now),
        )
        db.commit()
    return {"device_id": device_id, "revision": policy["revision"],
            "published_at": now, "status": "published"}


@router.post("/v1/admin/devices/{device_id}/revoke")
def revoke_device(device_id: str, authorization: str | None = Header(default=None)):
    _admin(authorization)
    _device_id(device_id)
    api = _api()
    now = int(time.time())
    with api.database() as db:
        db.execute("BEGIN IMMEDIATE")
        cursor = db.execute(
            "UPDATE devices SET revoked_at=? WHERE device_id=? AND revoked_at IS NULL",
            (now, device_id),
        )
        db.execute("DELETE FROM access_tokens WHERE device_id=?", (device_id,))
        # Keep the TOTP configuration for audit/re-enrollment policy, but
        # revoke any active authorization immediately.
        db.execute(
            "UPDATE parent_auth SET grant_expires_at=0,updated_at=? WHERE device_id=?",
            (now, device_id),
        )
        db.commit()
    if cursor.rowcount != 1:
        raise HTTPException(404, "device not found or already revoked")
    return {"device_id": device_id, "revoked": True, "revoked_at": now}


@router.get("/v1/admin/devices/{device_id}/events")
def read_events(
    device_id: str, limit: int = 100,
    authorization: str | None = Header(default=None),
):
    _admin(authorization)
    _device_id(device_id)
    if not 1 <= limit <= 500:
        raise HTTPException(422, "limit must be between 1 and 500")
    api = _api()
    with api.database() as db:
        rows = db.execute(
            """SELECT event_id,sequence,event_type,event_json,received_at
               FROM events WHERE device_id=? ORDER BY sequence DESC LIMIT ?""",
            (device_id, limit),
        ).fetchall()
    return {"device_id": device_id, "events": [
        {"event_id": row["event_id"], "sequence": row["sequence"],
         "type": row["event_type"], "event": json.loads(row["event_json"]),
         "received_at": row["received_at"]} for row in rows
    ]}
