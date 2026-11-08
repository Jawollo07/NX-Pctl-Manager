#!/usr/bin/env python3
"""Create the fixed-size one-time bootstrap payload consumed by nxpctl-bootstrap.nro."""
from __future__ import annotations

import argparse
import getpass
import re
import struct
from pathlib import Path

API_HOST_CAPACITY = 254
DEVICE_ID_CAPACITY = 65
ENROLLMENT_CODE_CAPACITY = 129
PAYLOAD_SIZE = API_HOST_CAPACITY + DEVICE_ID_CAPACITY + ENROLLMENT_CODE_CAPACITY


def build_payload(api_host: str, device_id: str, enrollment_code: str) -> bytes:
    if not re.fullmatch(r"[A-Za-z0-9](?:[A-Za-z0-9.-]{0,252}[A-Za-z0-9])?", api_host):
        raise ValueError("API host must be a hostname only (no scheme, port, path or underscore)")
    if len(api_host) >= API_HOST_CAPACITY:
        raise ValueError("API host is too long")
    if not re.fullmatch(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}", device_id):
        raise ValueError("invalid device ID")
    if not re.fullmatch(r"[A-Za-z0-9_-]{20,128}", enrollment_code):
        raise ValueError("enrollment code must be 20..128 URL-safe characters")

    payload = struct.pack(
        f"<{API_HOST_CAPACITY}s{DEVICE_ID_CAPACITY}s{ENROLLMENT_CODE_CAPACITY}s",
        api_host.encode("ascii"),
        device_id.encode("ascii"),
        enrollment_code.encode("ascii"),
    )
    if len(payload) != PAYLOAD_SIZE:
        raise AssertionError("provisioning payload layout mismatch")
    return payload


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create a one-time NX-Pctl-Manager HIPC bootstrap file."
    )
    parser.add_argument(
        "-o", "--output", type=Path, default=Path("nxpctl-provision.bin"),
        help="output file (default: ./nxpctl-provision.bin)",
    )
    args = parser.parse_args()

    host = input("Backend API hostname (without http://, port or path): ").strip()
    device_id = input("Device ID: ").strip()
    code = getpass.getpass("One-time enrollment code: ").strip()
    try:
        payload = build_payload(host, device_id, code)
    except (UnicodeEncodeError, ValueError) as exc:
        parser.error(str(exc))

    args.output.write_bytes(payload)
    args.output.chmod(0o600)
    print(f"Created {args.output} ({len(payload)} bytes). Copy it to the SD-card root.")
    print("The bootstrap app deletes the file after the sysmodule accepts the request.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
