"""NX-Pctl central control API.

Deploy behind a TLS-terminating reverse proxy. Device credentials are random
bearer tokens; only SHA-256 hashes are stored. The enrollment code is one-time.
No PIN, TOTP secret, or Nintendo account credential is accepted by this API.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import os
import secrets
import sqlite3
import time
from contextlib import contextmanager
from typing import Any

from fastapi import Depends, FastAPI, Header, HTTPException, status
from pydantic import BaseModel, Field, ConfigDict

DB_PATH = os.getenv("CONTROL_DB_PATH", "/data/control.db")
ADMIN_TOKEN = os.getenv("CONTROL_ADMIN_TOKEN", "")
INITIAL_ENROLLMENT_CODE = os.getenv("CONTROL_ENROLLMENT_CODE", "")

app = FastAPI(title="NX-Pctl Central Control API", version="1.0.0",
              docs_url=None, redoc_url=None, openapi_url=None)


@contextmanager
def db():
    os.makedirs(os.path.dirname(os.path.abspath(DB_PATH)), exist_ok=True)
    conn = sqlite3.connect(DB_PATH, timeout=10)
    conn.row_factory = sqlite3.Row
    try:
        conn.execute("PRAGMA journal_mode=WAL")
        conn.execute("PRAGMA foreign_keys=ON")
        yield conn
        conn.commit()
    except Exception:
        conn.rollback()
        raise
    finally:
        conn.close()


def init_db() -> None:
    with db() as conn:
        conn.executescript("""
        CREATE TABLE IF NOT EXISTS devices (
            device_id TEXT PRIMARY KEY,
            name TEXT NOT NULL,
            token_hash TEXT NOT NULL UNIQUE,
            created_at INTEGER NOT NULL,
            last_seen INTEGER,
            client_version TEXT,
            last_status_json TEXT NOT NULL DEFAULT '{}',
            enabled INTEGER NOT NULL DEFAULT 1
        );
        CREATE TABLE IF NOT EXISTS policies (
            device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
            version INTEGER NOT NULL DEFAULT 1,
            policy_json TEXT NOT NULL,
            updated_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS enrollment (
            singleton INTEGER PRIMARY KEY CHECK(singleton = 1),
            code_hash TEXT NOT NULL,
            consumed INTEGER NOT NULL DEFAULT 0
        );
        """)
        if INITIAL_ENROLLMENT_CODE:
            row = conn.execute("SELECT singleton FROM enrollment WHERE singleton=1").fetchone()
            if row is None:
                conn.execute(
                    "INSERT INTO enrollment(singleton, code_hash, consumed) VALUES(1, ?, 0)",
                    (hashlib.sha256(INITIAL_ENROLLMENT_CODE.encode()).hexdigest(),),
                )


@app.on_event("startup")
def startup() -> None:
    init_db()


class EnrollRequest(BaseModel):
    enrollment_code: str = Field(min_length=12, max_length=256)
    device_name: str = Field(min_length=1, max_length=80)
    client_version: str = Field(default="unknown", max_length=40)


class EnrollResponse(BaseModel):
    device_id: str
    device_token: str
    policy_version: int
    policy: dict[str, Any]


class DeviceStatus(BaseModel):
    model_config = ConfigDict(extra="forbid")
    client_version: str = Field(default="unknown", max_length=40)
    app_version: str = Field(default="unknown", max_length=40)
    online: bool = True
    last_sync_unix: int | None = None
    policy_version: int = Field(default=0, ge=0)
    supported_features: list[str] = Field(default_factory=list, max_length=64)
    applied_features: list[str] = Field(default_factory=list, max_length=64)
    warnings: list[str] = Field(default_factory=list, max_length=64)


class PolicyUpdate(BaseModel):
    # This API stores desired policy only. It does not execute arbitrary commands.
    daily_limit_minutes: int | None = Field(default=None, ge=0, le=1440)
    enabled: bool = True
    bedtime_start: str | None = Field(default=None, pattern=r"^(?:[01]\d|2[0-3]):[0-5]\d$")
    bedtime_end: str | None = Field(default=None, pattern=r"^(?:[01]\d|2[0-3]):[0-5]\d$")
    note: str = Field(default="", max_length=500)


def require_admin(authorization: str | None = Header(default=None)) -> None:
    if not ADMIN_TOKEN:
        raise HTTPException(status_code=503, detail="Admin API is not configured")
    supplied = authorization.removeprefix("Bearer ").strip() if authorization else ""
    if not hmac.compare_digest(supplied, ADMIN_TOKEN):
        raise HTTPException(status_code=401, detail="Invalid admin credentials")


def device_from_token(authorization: str | None = Header(default=None)) -> sqlite3.Row:
    supplied = authorization.removeprefix("Bearer ").strip() if authorization else ""
    if not supplied or len(supplied) > 256:
        raise HTTPException(status_code=401, detail="Missing device credentials")
    token_hash = hashlib.sha256(supplied.encode()).hexdigest()
    with db() as conn:
        row = conn.execute(
            "SELECT * FROM devices WHERE token_hash=? AND enabled=1", (token_hash,)
        ).fetchone()
    if row is None:
        raise HTTPException(status_code=401, detail="Invalid device credentials")
    return row


@app.get("/health")
def health() -> dict[str, str]:
    return {"status": "ok", "service": "nx-pctl-control"}


@app.post("/api/v1/enroll", response_model=EnrollResponse, status_code=201)
def enroll(body: EnrollRequest) -> EnrollResponse:
    code_hash = hashlib.sha256(body.enrollment_code.encode()).hexdigest()
    token = secrets.token_urlsafe(32)
    device_id = secrets.token_hex(16)
    now = int(time.time())
    default_policy = {"enabled": True, "daily_limit_minutes": None,
                      "bedtime_start": None, "bedtime_end": None, "note": ""}
    with db() as conn:
        row = conn.execute(
            "SELECT code_hash, consumed FROM enrollment WHERE singleton=1"
        ).fetchone()
        if row is None or row["consumed"] or not hmac.compare_digest(row["code_hash"], code_hash):
            raise HTTPException(status_code=403, detail="Enrollment code invalid or already used")
        conn.execute("UPDATE enrollment SET consumed=1 WHERE singleton=1 AND consumed=0")
        if conn.execute("SELECT changes()").fetchone()[0] != 1:
            raise HTTPException(status_code=409, detail="Enrollment code already consumed")
        conn.execute(
            "INSERT INTO devices(device_id,name,token_hash,created_at,client_version) VALUES(?,?,?,?,?)",
            (device_id, body.device_name, hashlib.sha256(token.encode()).hexdigest(),
             now, body.client_version),
        )
        conn.execute(
            "INSERT INTO policies(device_id,version,policy_json,updated_at) VALUES(?,1,?,?)",
            (device_id, json.dumps(default_policy), now),
        )
    # Token is returned exactly once. It cannot be recovered from the database.
    return EnrollResponse(device_id=device_id, device_token=token,
                          policy_version=1, policy=default_policy)


@app.get("/api/v1/policy")
def get_policy(device: sqlite3.Row = Depends(device_from_token)) -> dict[str, Any]:
    with db() as conn:
        row = conn.execute(
            "SELECT version, policy_json, updated_at FROM policies WHERE device_id=?",
            (device["device_id"],),
        ).fetchone()
        conn.execute("UPDATE devices SET last_seen=? WHERE device_id=?",
                     (int(time.time()), device["device_id"]))
    if row is None:
        raise HTTPException(status_code=404, detail="Policy not found")
    return {"device_id": device["device_id"], "version": row["version"],
            "updated_at": row["updated_at"], "policy": json.loads(row["policy_json"])}


@app.post("/api/v1/status", status_code=204)
def report_status(body: DeviceStatus, device: sqlite3.Row = Depends(device_from_token)) -> None:
    with db() as conn:
        conn.execute(
            "UPDATE devices SET last_seen=?, client_version=?, last_status_json=? WHERE device_id=?",
            (int(time.time()), body.client_version, body.model_dump_json(), device["device_id"]),
        )


@app.get("/api/v1/admin/devices", dependencies=[Depends(require_admin)])
def list_devices() -> list[dict[str, Any]]:
    with db() as conn:
        rows = conn.execute(
            "SELECT d.device_id,d.name,d.created_at,d.last_seen,d.client_version,d.enabled,"
            "d.last_status_json,p.version AS policy_version FROM devices d "
            "LEFT JOIN policies p ON p.device_id=d.device_id ORDER BY d.created_at DESC"
        ).fetchall()
    result = []
    for row in rows:
        item = dict(row)
        item["last_status"] = json.loads(item.pop("last_status_json"))
        result.append(item)
    return result


@app.put("/api/v1/admin/devices/{device_id}/policy", dependencies=[Depends(require_admin)])
def update_policy(device_id: str, body: PolicyUpdate) -> dict[str, Any]:
    now = int(time.time())
    payload = body.model_dump()
    with db() as conn:
        exists = conn.execute("SELECT 1 FROM devices WHERE device_id=?", (device_id,)).fetchone()
        if exists is None:
            raise HTTPException(status_code=404, detail="Device not found")
        conn.execute(
            "INSERT INTO policies(device_id,version,policy_json,updated_at) VALUES(?,1,?,?) "
            "ON CONFLICT(device_id) DO UPDATE SET version=policies.version+1,"
            "policy_json=excluded.policy_json,updated_at=excluded.updated_at",
            (device_id, json.dumps(payload), now),
        )
        row = conn.execute("SELECT version FROM policies WHERE device_id=?", (device_id,)).fetchone()
    return {"device_id": device_id, "version": row["version"], "updated_at": now, "policy": payload}


@app.post("/api/v1/admin/devices/{device_id}/disable", dependencies=[Depends(require_admin)])
def disable_device(device_id: str) -> dict[str, str]:
    with db() as conn:
        cur = conn.execute("UPDATE devices SET enabled=0 WHERE device_id=?", (device_id,))
        if cur.rowcount == 0:
            raise HTTPException(status_code=404, detail="Device not found")
    return {"status": "disabled"}
