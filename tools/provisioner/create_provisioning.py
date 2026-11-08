#!/usr/bin/env python3
"""Create the one-time nxpctl.conf file consumed by nxpctl-bootstrap.nro."""
from __future__ import annotations

import argparse
import getpass
import re
from pathlib import Path
from urllib.parse import urlsplit

HOST_RE = re.compile(r"(?=.{1,253}\Z)(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?)(?:\.(?:[A-Za-z0-9](?:[A-Za-z0-9-]{0,61}[A-Za-z0-9])?))*")
DEVICE_RE = re.compile(r"[A-Za-z0-9][A-Za-z0-9._-]{0,63}")
CODE_RE = re.compile(r"[A-Za-z0-9_-]{20,128}")


def validate_api_url(api_url: str) -> None:
    try:
        parsed = urlsplit(api_url)
        port = parsed.port
    except ValueError as exc:
        raise ValueError(f"invalid backend URL: {exc}") from exc

    if parsed.scheme != "http" or not parsed.netloc:
        raise ValueError("backend URL must start with http://")
    if parsed.username is not None or parsed.password is not None:
        raise ValueError("credentials in backend URL are not allowed")
    if parsed.path not in ("", "/") or parsed.query or parsed.fragment:
        raise ValueError("enter only the base address, without a path, query or fragment")
    if not parsed.hostname or not HOST_RE.fullmatch(parsed.hostname):
        raise ValueError("backend URL must contain a valid DNS hostname or IPv4 address")
    if len(api_url) >= 254:
        raise ValueError("backend URL is too long")
    if port is not None and not (1 <= port <= 65535):
        raise ValueError("port must be between 1 and 65535")


def build_config(api_url: str, device_id: str, enrollment_code: str) -> str:
    validate_api_url(api_url)
    if not DEVICE_RE.fullmatch(device_id):
        raise ValueError("invalid device ID")
    if not CODE_RE.fullmatch(enrollment_code):
        raise ValueError("enrollment code must be 20..128 URL-safe characters")
    return (
        "# NX-Pctl-Manager one-time device enrollment\n"
        f"api_url={api_url.rstrip('/')}\n"
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

    api_url = input("Backend address (e.g. http://192.168.178.10:1212): ").strip()
    device_id = input("Device ID: ").strip()
    code = getpass.getpass("One-time enrollment code: ").strip()
    try:
        config = build_config(api_url, device_id, code)
    except ValueError as exc:
        parser.error(str(exc))

    args.output.write_text(config, encoding="ascii", newline="\n")
    args.output.chmod(0o600)
    print(f"Created {args.output}. Copy it to the root of the Switch SD card.")
    print("The bootstrap app deletes nxpctl.conf after the sysmodule accepts it.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
