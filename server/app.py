"""NX-Pctl-Manager reference backend.

SQLite is authoritative server-side storage. The Switch is expected to keep
policy snapshots and access tokens in RAM only. This service deliberately
supports plain HTTP for isolated development networks; use a trusted TLS
reverse proxy for any untrusted network.
"""
from __future__ import annotations

import hashlib
import hmac
import json
import os
import re
import secrets
import sqlite3
import time
from contextlib import contextmanager
from pathlib import Path
from typing import Any

from fastapi import FastAPI, HTTPException, Request
from fastapi.responses import JSONResponse

APP_VERSION = "0.1.0"
PROTOCOL_VERSION = 1
MAX_REQUEST_BYTES = 96 * 1024
MAX_EVENT_BATCH = 100
MAX_EVENT_PAYLOAD_BYTES = 4096
TOKEN_TTL_SECONDS = 15 * 60
ENROLLMENT_TTL_SECONDS = 30 * 60
DEVICE_ID_RE = re.compile(r"^[A-Za-z0-9][A-Za-z0-9._-]{0,63}$")
POLICY_FIELDS = {
    "protocol_version", "schema_version", "device_id", "revision",
    "issued_at", "not_after", "server_time", "enabled",
    "require_parent_authentication", "offline_behavior",
    "timezone_offset_minutes", "global_daily_limit_minutes",
    "weekday_limit_minutes", "quiet_hours_enabled",
    "quiet_hours_start_minute", "quiet_hours_end_minute",
    "title_rule_mode", "title_rules",
}


class InvalidJSON(ValueError):
    pass


def _unique_object(pairs: list[tuple[str, Any]]) -> dict[str, Any]:
    result: dict[str, Any] = {}
    for key, value in pairs:
        if key in result:
            raise InvalidJSON(f"duplicate JSON field: {key}")
        result[key] = value
    return result


def _check_json_depth(value: Any, depth: int = 0) -> None:
    if depth > 8:
        raise InvalidJSON("JSON nesting exceeds eight levels")
    if isinstance(value, dict):
        for child in value.values():
            _check_json_depth(child, depth + 1)
    elif isinstance(value, list):
        for child in value:
            _check_json_depth(child, depth + 1)


def _parse_integer(text: str) -> int:
    digits = text[1:] if text.startswith("-") else text
    if len(digits) > 20:
        raise InvalidJSON("integer literal is too large")
    return int(text)


def strict_json_loads(raw: bytes) -> Any:
    if len(raw) > MAX_REQUEST_BYTES:
        raise InvalidJSON("request body too large")
    try:
        text = raw.decode("utf-8", errors="strict")
        value = json.loads(
            text,
            object_pairs_hook=_unique_object,
            parse_int=_parse_integer,
            parse_float=lambda _: (_ for _ in ()).throw(InvalidJSON("floats are not accepted")),
            parse_constant=lambda _: (_ for _ in ()).throw(InvalidJSON("non-finite numbers are not accepted")),
        )
        _check_json_depth(value)
        return value
    except (UnicodeDecodeError, json.JSONDecodeError, RecursionError) as exc:
        raise InvalidJSON("invalid or excessively nested JSON") from exc


def _is_int(value: Any) -> bool:
    return isinstance(value, int) and not isinstance(value, bool)


def _require_object(value: Any) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise HTTPException(400, "JSON object required")
    return value


def _validate_policy(policy: Any, expected_device_id: str | None = None) -> dict[str, Any]:
    obj = _require_object(policy)
    if set(obj) != POLICY_FIELDS:
        missing = sorted(POLICY_FIELDS - set(obj))
        unknown = sorted(set(obj) - POLICY_FIELDS)
        raise HTTPException(422, {"error": "policy_fields", "missing": missing, "unknown": unknown})

    integer_fields = (
        "protocol_version", "schema_version", "revision", "issued_at",
        "not_after", "server_time", "offline_behavior",
        "timezone_offset_minutes", "global_daily_limit_minutes",
        "quiet_hours_start_minute", "quiet_hours_end_minute", "title_rule_mode",
    )
    if any(not _is_int(obj[name]) for name in integer_fields):
        raise HTTPException(422, "policy integer field has wrong type")
    boolean_fields = ("enabled", "require_parent_authentication", "quiet_hours_enabled")
    if any(not isinstance(obj[name], bool) for name in boolean_fields):
        raise HTTPException(422, "policy boolean field has wrong type")
    if obj["protocol_version"] != 1 or obj["schema_version"] != 1:
        raise HTTPException(422, "unsupported policy protocol/schema version")
    device_id = obj["device_id"]
    if not isinstance(device_id, str) or not DEVICE_ID_RE.fullmatch(device_id):
        raise HTTPException(422, "invalid policy device_id")
    if expected_device_id is not None and device_id != expected_device_id:
        raise HTTPException(403, "policy device_id mismatch")
    if not 1 <= obj["revision"] <= 0xFFFFFFFFFFFFFFFF:
        raise HTTPException(422, "revision must be a positive unsigned 64-bit integer")
    if any(not 0 <= obj[name] <= 0xFFFFFFFFFFFFFFFF for name in ("issued_at", "not_after", "server_time")):
        raise HTTPException(422, "policy timestamps must be unsigned 64-bit integers")
    now = int(time.time())
    if obj["issued_at"] < 0 or obj["not_after"] <= obj["issued_at"]:
        raise HTTPException(422, "invalid policy validity interval")
    if obj["server_time"] < 0 or abs(obj["server_time"] - now) > 300:
        raise HTTPException(422, "server_time must be within five minutes of server clock")
    if obj["issued_at"] > now + 300 or obj["not_after"] <= now:
        raise HTTPException(422, "policy is not currently valid")
    if obj["not_after"] - obj["issued_at"] > 7 * 24 * 60 * 60:
        raise HTTPException(422, "policy lifetime exceeds seven days")
    if obj["offline_behavior"] not in (0, 1, 2):
        raise HTTPException(422, "invalid offline_behavior")
    if not -840 <= obj["timezone_offset_minutes"] <= 840:
        raise HTTPException(422, "invalid timezone_offset_minutes")

    def valid_limit(v: Any) -> bool:
        return _is_int(v) and (0 <= v <= 1440 or v == 65535)

    if not valid_limit(obj["global_daily_limit_minutes"]):
        raise HTTPException(422, "invalid global_daily_limit_minutes")
    weekday = obj["weekday_limit_minutes"]
    if not isinstance(weekday, list) or len(weekday) != 7 or not all(valid_limit(v) for v in weekday):
        raise HTTPException(422, "weekday_limit_minutes must contain seven valid limits")
    start, end = obj["quiet_hours_start_minute"], obj["quiet_hours_end_minute"]
    if obj["quiet_hours_enabled"]:
        if not (0 <= start < 1440 and 0 <= end < 1440 and start != end):
            raise HTTPException(422, "invalid quiet-hours interval")
    elif not (0 <= start <= 1439 and 0 <= end <= 1439):
        raise HTTPException(422, "quiet-hours minute out of range")
    if obj["title_rule_mode"] not in (0, 1):
        raise HTTPException(422, "invalid title_rule_mode")
    rules = obj["title_rules"]
    if not isinstance(rules, list) or len(rules) > 64:
        raise HTTPException(422, "title_rules must contain at most 64 entries")
    seen: set[int] = set()
    for rule in rules:
        if not isinstance(rule, dict) or set(rule) != {"title_id", "action"}:
            raise HTTPException(422, "each title rule needs title_id and action only")
        title_id, action = rule["title_id"], rule["action"]
        if not _is_int(title_id) or not 1 <= title_id <= 0xFFFFFFFFFFFFFFFF:
            raise HTTPException(422, "invalid title_id")
        if not _is_int(action) or action not in (0, 1):
            raise HTTPException(422, "invalid title rule action")
        if title_id in seen:
            raise HTTPException(422, "duplicate title_id")
        seen.add(title_id)
    return obj


def _hash_secret(value: str) -> str:
    return hashlib.sha256(value.encode("utf-8")).hexdigest()


def _database_path() -> Path:
    return Path(os.environ.get("NXPCTL_DATABASE", "./data/nxpctl.sqlite3")).expanduser()


@contextmanager
def database():
    path = _database_path()
    path.parent.mkdir(parents=True, exist_ok=True)
    connection = sqlite3.connect(path, timeout=10, isolation_level=None)
    connection.row_factory = sqlite3.Row
    connection.execute("PRAGMA foreign_keys = ON")
    connection.execute("PRAGMA journal_mode = WAL")
    try:
        yield connection
    finally:
        connection.close()


def init_database() -> None:
    with database() as db:
        db.executescript("""
        CREATE TABLE IF NOT EXISTS devices (
            device_id TEXT PRIMARY KEY,
            credential_hash TEXT NOT NULL,
            created_at INTEGER NOT NULL,
            revoked_at INTEGER,
            last_auth_at INTEGER,
            last_seen_at INTEGER,
            last_heartbeat_json TEXT
        );
        CREATE TABLE IF NOT EXISTS enrollment_codes (
            code_hash TEXT PRIMARY KEY,
            device_id TEXT NOT NULL UNIQUE,
            expires_at INTEGER NOT NULL,
            used_at INTEGER
        );
        CREATE TABLE IF NOT EXISTS access_tokens (
            token_hash TEXT PRIMARY KEY,
            device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
            expires_at INTEGER NOT NULL,
            created_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS policies (
            device_id TEXT PRIMARY KEY REFERENCES devices(device_id) ON DELETE CASCADE,
            revision INTEGER NOT NULL,
            body_json TEXT NOT NULL,
            updated_at INTEGER NOT NULL
        );
        CREATE TABLE IF NOT EXISTS events (
            device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
            event_id TEXT NOT NULL,
            sequence INTEGER NOT NULL,
            event_type TEXT NOT NULL,
            event_json TEXT NOT NULL,
            received_at INTEGER NOT NULL,
            PRIMARY KEY(device_id, event_id),
            UNIQUE(device_id, sequence)
        );
        CREATE TABLE IF NOT EXISTS policy_acks (
            device_id TEXT NOT NULL REFERENCES devices(device_id) ON DELETE CASCADE,
            revision INTEGER NOT NULL,
            status TEXT NOT NULL,
            message TEXT,
            received_at INTEGER NOT NULL,
            PRIMARY KEY(device_id, revision)
        );
        """)


app = FastAPI(title="NX-Pctl-Manager API", version=APP_VERSION)


@app.on_event("startup")
def startup() -> None:
    init_database()
    if os.environ.get("NXPCTL_PUBLIC_HTTP", "0") == "1":
        print("WARNING: public plaintext HTTP is enabled; device credentials and tokens are exposed in transit.")


@app.middleware("http")
async def limit_request_size(request: Request, call_next):
    length = request.headers.get("content-length")
    if length:
        try:
            if int(length) > MAX_REQUEST_BYTES:
                return JSONResponse(status_code=413, content={"detail": "request body too large"})
        except ValueError:
            return JSONResponse(status_code=400, content={"detail": "invalid content-length"})
    response = await call_next(request)
    return response


async def read_json(request: Request) -> Any:
    raw = await request.body()
    try:
        return strict_json_loads(raw)
    except InvalidJSON as exc:
        raise HTTPException(400, str(exc)) from exc


def authenticated_device(request: Request) -> str:
    header = request.headers.get("authorization", "")
    scheme, _, token = header.partition(" ")
    if scheme.lower() != "bearer" or not token or len(token) > 256:
        raise HTTPException(401, "bearer token required", headers={"WWW-Authenticate": "Bearer"})
    now = int(time.time())
    with database() as db:
        row = db.execute("""
            SELECT t.device_id, d.revoked_at
            FROM access_tokens t JOIN devices d USING(device_id)
            WHERE t.token_hash = ? AND t.expires_at > ?
        """, (_hash_secret(token), now)).fetchone()
        if row is None or row["revoked_at"] is not None:
            raise HTTPException(401, "invalid or expired bearer token")
        db.execute("UPDATE devices SET last_seen_at = ? WHERE device_id = ?", (now, row["device_id"]))
        return str(row["device_id"])


@app.get("/healthz")
def healthz():
    return {"status": "ok", "service": "nx-pctl-manager", "version": APP_VERSION}


@app.post("/v1/devices/enroll")
async def enroll(request: Request):
    body = _require_object(await read_json(request))
    code = body.get("enrollment_code")
    device_id = body.get("device_id")
    if not isinstance(code, str) or not 20 <= len(code) <= 128:
        raise HTTPException(400, "invalid enrollment_code")
    if not isinstance(device_id, str) or not DEVICE_ID_RE.fullmatch(device_id):
        raise HTTPException(400, "invalid device_id")
    now = int(time.time())
    credential = secrets.token_urlsafe(32)
    with database() as db:
        db.execute("BEGIN IMMEDIATE")
        enrollment = db.execute(
            "SELECT device_id, expires_at, used_at FROM enrollment_codes WHERE code_hash = ?",
            (_hash_secret(code),),
        ).fetchone()
        if enrollment is None or enrollment["device_id"] != device_id or enrollment["used_at"] is not None or enrollment["expires_at"] <= now:
            db.rollback()
            raise HTTPException(403, "enrollment code invalid, expired, or already used")
        existing = db.execute("SELECT revoked_at FROM devices WHERE device_id = ?", (device_id,)).fetchone()
        if existing is not None and existing["revoked_at"] is None:
            db.rollback()
            raise HTTPException(409, "device_id is already enrolled")
        if existing is not None:
            # Re-enrollment rotates identity and starts a clean policy/event epoch.
            db.execute("DELETE FROM access_tokens WHERE device_id = ?", (device_id,))
            db.execute("DELETE FROM policies WHERE device_id = ?", (device_id,))
            db.execute("DELETE FROM events WHERE device_id = ?", (device_id,))
            db.execute("DELETE FROM policy_acks WHERE device_id = ?", (device_id,))
            db.execute(
                """UPDATE devices SET credential_hash=?,created_at=?,revoked_at=NULL,
                   last_auth_at=NULL,last_seen_at=NULL,last_heartbeat_json=NULL
                   WHERE device_id=?""",
                (_hash_secret(credential), now, device_id),
            )
        else:
            db.execute(
                "INSERT INTO devices(device_id, credential_hash, created_at) VALUES(?,?,?)",
                (device_id, _hash_secret(credential), now),
            )
        db.execute("UPDATE enrollment_codes SET used_at = ? WHERE code_hash = ?", (now, _hash_secret(code)))
        db.commit()
    return {"device_id": device_id, "device_credential": credential, "created_at": now}


@app.post("/v1/devices/auth")
async def authenticate(request: Request):
    body = _require_object(await read_json(request))
    device_id, credential = body.get("device_id"), body.get("device_credential")
    if not isinstance(device_id, str) or not DEVICE_ID_RE.fullmatch(device_id):
        raise HTTPException(400, "invalid device_id")
    if not isinstance(credential, str) or not 32 <= len(credential) <= 128:
        raise HTTPException(401, "invalid device credentials")
    now = int(time.time())
    with database() as db:
        row = db.execute(
            "SELECT credential_hash, revoked_at FROM devices WHERE device_id = ?",
            (device_id,),
        ).fetchone()
        supplied_hash = _hash_secret(credential)
        if row is None or row["revoked_at"] is not None or not hmac.compare_digest(row["credential_hash"], supplied_hash):
            raise HTTPException(401, "invalid device credentials")
        token = secrets.token_urlsafe(32)
        expires = now + TOKEN_TTL_SECONDS
        db.execute("DELETE FROM access_tokens WHERE expires_at <= ?", (now,))
        db.execute(
            "INSERT INTO access_tokens(token_hash, device_id, expires_at, created_at) VALUES(?,?,?,?)",
            (_hash_secret(token), device_id, expires, now),
        )
        db.execute("UPDATE devices SET last_auth_at = ?, last_seen_at = ? WHERE device_id = ?", (now, now, device_id))
    return {"access_token": token, "token_type": "Bearer", "expires_at": expires, "device_id": device_id, "server_time": now}


@app.get("/v1/devices/{device_id}/policy")
def get_policy(device_id: str, request: Request):
    authenticated = authenticated_device(request)
    if authenticated != device_id:
        raise HTTPException(403, "device scope mismatch")
    with database() as db:
        row = db.execute("SELECT body_json FROM policies WHERE device_id = ?", (device_id,)).fetchone()
    if row is None:
        raise HTTPException(404, "no policy configured for this device")
    policy = json.loads(row["body_json"])
    now = int(time.time())
    if policy["not_after"] <= now:
        raise HTTPException(410, "configured policy has expired")
    # server_time is a response-time clock sample, not a publish-time field.
    policy["server_time"] = now
    return JSONResponse(content=policy, headers={"Cache-Control": "no-store"})


@app.post("/v1/devices/{device_id}/heartbeat")
async def heartbeat(device_id: str, request: Request):
    if authenticated_device(request) != device_id:
        raise HTTPException(403, "device scope mismatch")
    body = _require_object(await read_json(request))
    if len(body) > 32:
        raise HTTPException(422, "too many heartbeat fields")
    for key in ("protocol_version", "uptime_seconds", "policy_revision"):
        if key in body and (not _is_int(body[key]) or body[key] < 0):
            raise HTTPException(422, f"invalid {key}")
    if body.get("protocol_version", PROTOCOL_VERSION) != PROTOCOL_VERSION:
        raise HTTPException(422, "unsupported protocol_version")
    serialized = json.dumps(body, separators=(",", ":"), ensure_ascii=True)
    with database() as db:
        db.execute(
            "UPDATE devices SET last_seen_at = ?, last_heartbeat_json = ? WHERE device_id = ?",
            (int(time.time()), serialized, device_id),
        )
    return {"accepted": True, "server_time": int(time.time())}


@app.post("/v1/devices/{device_id}/events:batch")
async def events_batch(device_id: str, request: Request):
    if authenticated_device(request) != device_id:
        raise HTTPException(403, "device scope mismatch")
    body = _require_object(await read_json(request))
    events = body.get("events")
    if not isinstance(events, list) or not 1 <= len(events) <= MAX_EVENT_BATCH:
        raise HTTPException(422, "events must contain 1..100 entries")
    now = int(time.time())
    accepted = 0
    with database() as db:
        db.execute("BEGIN IMMEDIATE")
        for event in events:
            if not isinstance(event, dict) or set(event) != {"event_id", "sequence", "timestamp", "type", "payload"}:
                db.rollback()
                raise HTTPException(422, "event fields must be event_id, sequence, timestamp, type, payload")
            event_id, sequence, timestamp = event["event_id"], event["sequence"], event["timestamp"]
            event_type, payload = event["type"], event["payload"]
            if not isinstance(event_id, str) or not 1 <= len(event_id) <= 64:
                db.rollback(); raise HTTPException(422, "invalid event_id")
            if not _is_int(sequence) or not 0 <= sequence <= 0xFFFFFFFFFFFFFFFF or not _is_int(timestamp) or not 0 <= timestamp <= 0xFFFFFFFFFFFFFFFF:
                db.rollback(); raise HTTPException(422, "invalid event sequence/timestamp")
            if not isinstance(event_type, str) or not 1 <= len(event_type) <= 64 or not re.fullmatch(r"[a-zA-Z0-9._-]+", event_type):
                db.rollback(); raise HTTPException(422, "invalid event type")
            payload_json = json.dumps(payload, separators=(",", ":"), ensure_ascii=True)
            if len(payload_json.encode("utf-8")) > MAX_EVENT_PAYLOAD_BYTES:
                db.rollback(); raise HTTPException(422, "event payload too large")
            event_json = json.dumps(event, separators=(",", ":"), ensure_ascii=True)
            cursor = db.execute(
                "INSERT OR IGNORE INTO events(device_id,event_id,sequence,event_type,event_json,received_at) VALUES(?,?,?,?,?,?)",
                (device_id, event_id, sequence, event_type, event_json, now),
            )
            accepted += cursor.rowcount
        db.commit()
    return {"accepted": accepted, "received": len(events), "server_time": now}


@app.post("/v1/devices/{device_id}/policy-ack")
async def policy_ack(device_id: str, request: Request):
    if authenticated_device(request) != device_id:
        raise HTTPException(403, "device scope mismatch")
    body = _require_object(await read_json(request))
    revision, status = body.get("revision"), body.get("status")
    message = body.get("message", "")
    if not _is_int(revision) or revision < 1:
        raise HTTPException(422, "invalid revision")
    if status not in ("accepted", "rejected", "applied", "failed"):
        raise HTTPException(422, "invalid status")
    if not isinstance(message, str) or len(message) > 256:
        raise HTTPException(422, "invalid message")
    with database() as db:
        db.execute(
            """INSERT INTO policy_acks(device_id,revision,status,message,received_at)
               VALUES(?,?,?,?,?) ON CONFLICT(device_id,revision) DO UPDATE SET
               status=excluded.status,message=excluded.message,received_at=excluded.received_at""",
            (device_id, revision, status, message, int(time.time())),
        )
    return {"accepted": True}


# Administrative routes are intentionally absent. Provisioning and policy
# changes happen through the local CLI, not through an unauthenticated web UI.
