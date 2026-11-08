#!/usr/bin/env python3
"""Create the one-time nxpctl.conf file consumed by nxpctl-bootstrap.nro."""
from __future__ import annotations

import argparse
import getpass
import re
from pathlib import Path

HOST_RE = re.compile(r"(?=.{1,253}\Z)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(?:\.(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?))*")
DEVICE_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
CODE_RE = re.compile(r"[A-Za-z0-9_-]{20,128}")


def build_config(api_host: str, device_id: str, enrollment_code: str) -> str:
    if not HOST_RE.fullmatch(api_host):
        raise ValueError("API host must be a hostname only (no scheme, port, path or underscore)")
    if len(api_host) >= 254:
        raise ValueError("API host is too long")
    if not DEVICE_RE.fullmatch(device_id):
        raise ValueError("invalid device ID")
    if not CODE_RE.fullmatch(enrollment_code):
        raise ValueError("enrollment code must be 20..128 URL-safe characters")
    return (
        "# NX-Pctl-Manager one-time device enrollment\n"
        f"api_host={api_host}\n"
        f"device_id={device_id}\n"
        f"enrollment_code={enrollment_code}\n"
    )


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Create the one-time NX-Pctl-Manager SD-card config."
    )
    parser.add_argument(
        "-o", "--output", type=Path, default=Path("nxpctl.conf"),
        help="output file (default: ./nxpctl.conf)",
    )
    args = parser.parse_args()

    host = input("Backend API hostname (without http://, port or path): ").strip()
    device_id = input("Device ID: ").strip()
    code = getpass.getpass("One-time enrollment code: ").strip()
    try:
        config = build_config(host, device_id, code)
    except ValueError as exc:
        parser.error(str(exc))

    args.output.write_text(config, encoding="ascii", newline="\n")
    args.output.chmod(0o600)
    print(f"Created {args.output}. Copy it to the root of the Switch SD card.")
    print("The bootstrap app deletes nxpctl.conf after the sysmodule accepts it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
