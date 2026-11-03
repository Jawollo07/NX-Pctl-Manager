"""Local administrator CLI for device enrollment and policy management."""
from __future__ import annotations

import argparse
import json
import secrets
import sys
import time
from pathlib import Path

from app import (
    DEVICE_ID_RE, _database_path, _hash_secret, _validate_policy,
    database, init_database, strict_json_loads,
)


def create_enrollment(args: argparse.Namespace) -> int:
    if not DEVICE_ID_RE.fullmatch(args.device_id):
        print("invalid device ID", file=sys.stderr)
        return 2
    init_database()
    now = int(time.time())
    code = secrets.token_urlsafe(24)
    with database() as db:
        existing = db.execute("SELECT revoked_at FROM devices WHERE device_id=?", (args.device_id,)).fetchone()
        if existing is not None and existing["revoked_at"] is None:
            print("device is already enrolled; revoke it before re-enrollment", file=sys.stderr)
            return 2
        db.execute("DELETE FROM enrollment_codes WHERE device_id=?", (args.device_id,))
        db.execute(
            "INSERT INTO enrollment_codes(code_hash,device_id,expires_at) VALUES(?,?,?)",
            (_hash_secret(code), args.device_id, now + args.ttl),
        )
    print(f"Device: {args.device_id}")
    print(f"Expires: {now + args.ttl}")
    print(f"Enrollment code (show once): {code}")
    return 0


def set_policy(args: argparse.Namespace) -> int:
    init_database()
    try:
        raw = Path(args.file).read_bytes()
        policy = strict_json_loads(raw)
    except (OSError, ValueError) as exc:
        print(f"cannot read policy JSON: {exc}", file=sys.stderr)
        return 2
    if not isinstance(policy, dict):
        print("policy must be a JSON object", file=sys.stderr)
        return 2
    policy["server_time"] = int(time.time())
    try:
        _validate_policy(policy, args.device_id)
    except Exception as exc:
        detail = getattr(exc, "detail", str(exc))
        print(f"policy rejected: {detail}", file=sys.stderr)
        return 2
    serialized = json.dumps(policy, separators=(",", ":"), ensure_ascii=True)
    with database() as db:
        if not db.execute("SELECT 1 FROM devices WHERE device_id=? AND revoked_at IS NULL", (args.device_id,)).fetchone():
            print("device does not exist or is revoked", file=sys.stderr)
            return 2
        current = db.execute("SELECT revision FROM policies WHERE device_id=?", (args.device_id,)).fetchone()
        if current and policy["revision"] <= current["revision"]:
            print(f"revision must be greater than current revision {current['revision']}", file=sys.stderr)
            return 2
        db.execute(
            """INSERT INTO policies(device_id,revision,body_json,updated_at) VALUES(?,?,?,?)
               ON CONFLICT(device_id) DO UPDATE SET revision=excluded.revision,
               body_json=excluded.body_json,updated_at=excluded.updated_at""",
            (args.device_id, policy["revision"], serialized, int(time.time())),
        )
    print(f"Published policy revision {policy['revision']} for {args.device_id}")
    return 0


def revoke_device(args: argparse.Namespace) -> int:
    init_database()
    with database() as db:
        cursor = db.execute(
            "UPDATE devices SET revoked_at=? WHERE device_id=? AND revoked_at IS NULL",
            (int(time.time()), args.device_id),
        )
        db.execute("DELETE FROM access_tokens WHERE device_id=?", (args.device_id,))
    if cursor.rowcount == 0:
        print("device not found or already revoked", file=sys.stderr)
        return 2
    print(f"Revoked {args.device_id}")
    return 0


def list_devices(_: argparse.Namespace) -> int:
    init_database()
    with database() as db:
        rows = db.execute(
            "SELECT device_id,created_at,revoked_at,last_auth_at,last_seen_at FROM devices ORDER BY device_id"
        ).fetchall()
    for row in rows:
        print(json.dumps(dict(row), sort_keys=True))
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(prog="nxpctl-server")
    parser.add_argument("--database", help="SQLite path (overrides NXPCTL_DATABASE)")
    sub = parser.add_subparsers(dest="command", required=True)

    enroll = sub.add_parser("create-enrollment", help="create a one-time enrollment code")
    enroll.add_argument("--device-id", required=True)
    enroll.add_argument("--ttl", type=int, default=1800, help="code lifetime in seconds (default: 1800)")
    enroll.set_defaults(func=create_enrollment)

    policy = sub.add_parser("set-policy", help="validate and publish a policy JSON file")
    policy.add_argument("--device-id", required=True)
    policy.add_argument("--file", required=True)
    policy.set_defaults(func=set_policy)

    revoke = sub.add_parser("revoke-device", help="revoke a device and its active tokens")
    revoke.add_argument("--device-id", required=True)
    revoke.set_defaults(func=revoke_device)

    listing = sub.add_parser("list-devices", help="list enrolled devices")
    listing.set_defaults(func=list_devices)

    args = parser.parse_args()
    if args.database:
        import os
        os.environ["NXPCTL_DATABASE"] = args.database
    if hasattr(args, "ttl") and not 60 <= args.ttl <= 86400:
        parser.error("--ttl must be between 60 and 86400 seconds")
    return args.func(args)


if __name__ == "__main__":
    raise SystemExit(main())
