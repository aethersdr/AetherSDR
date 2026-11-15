#!/usr/bin/env python3
"""Probe only the greeting and authorization reply of TGXL and PGXL.

The code is prompted without echo and is never stored or printed. This probe
does not send status, setup, tuning, or transmit commands.
"""

import argparse
import getpass
import socket
import sys


MAX_LINE = 4096
TIMEOUT_SECONDS = 5


def read_line(stream: socket.SocketIO) -> str:
    raw = stream.readline(MAX_LINE + 1)
    if not raw:
        raise ConnectionError("connection closed before a complete response")
    if len(raw) > MAX_LINE or not raw.endswith(b"\n"):
        raise ValueError("oversized or unterminated response")
    return raw.decode("ascii", errors="replace").strip()


def probe(name: str, host: str, port: int, code: str, auth_form: str) -> bool:
    print(f"{name} {host}:{port}", flush=True)
    try:
        with socket.create_connection((host, port), TIMEOUT_SECONDS) as connection:
            connection.settimeout(TIMEOUT_SECONDS)
            with connection.makefile("rb") as stream:
                greeting = read_line(stream)
                print(f"  greeting: {greeting.replace(code, '[REDACTED]')}", flush=True)
                if not greeting.startswith("V"):
                    print("  unexpected greeting; no command sent", flush=True)
                    return False
                if not greeting.endswith(" AUTH"):
                    print("  no AUTH challenge; no command sent", flush=True)
                    return True
                if auth_form == "skip":
                    print("  AUTH challenge present; auth syntax not selected", flush=True)
                    return False

                command = f"auth {code}" if auth_form == "space" else f"auth code={code}"
                print(f"  sending: C1|auth {'<code>' if auth_form == 'space' else 'code=<code>'}", flush=True)
                connection.sendall(f"C1|{command}\n".encode("ascii"))
                reply = read_line(stream).replace(code, "[REDACTED]")
                print(f"  reply: {reply}", flush=True)
                accepted = (reply in ("R0|0|auth OK", "R1|0|auth OK") if name == "TGXL"
                            else reply == "R1|0|Authorized")
                print(f"  auth accepted: {'yes' if accepted else 'no'}", flush=True)
                return accepted
    except (OSError, ConnectionError, ValueError) as error:
        print(f"  probe failed: {error}", flush=True)
        return False


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--tgxl-host")
    parser.add_argument("--tgxl-port", type=int, default=9010)
    parser.add_argument("--pgxl-host")
    parser.add_argument("--pgxl-port", type=int, default=9008)
    parser.add_argument("--pgxl-auth", choices=("skip", "space", "code-equals"),
                        default="code-equals", help="PGXL syntax to try once if challenged")
    parser.add_argument("--device", choices=("both", "tgxl", "pgxl"), default="both")
    args = parser.parse_args()
    if args.device in ("both", "tgxl") and not args.tgxl_host:
        parser.error("--tgxl-host is required for the selected device")
    if args.device in ("both", "pgxl") and not args.pgxl_host:
        parser.error("--pgxl-host is required for the selected device")
    if not 1 <= args.tgxl_port <= 65535 or not 1 <= args.pgxl_port <= 65535:
        parser.error("device ports must be between 1 and 65535")

    code = getpass.getpass("4O3A authorization code: ")
    if not code or len(code) > 128 or any(
        ord(character) <= 0x20 or ord(character) > 0x7E or character == "|"
        for character in code
    ):
        print("Authorization code has unsupported characters or length", file=sys.stderr)
        return 2

    success = True
    if args.device in ("both", "tgxl"):
        success = probe("TGXL", args.tgxl_host, args.tgxl_port, code, "space") and success
    if args.device in ("both", "pgxl"):
        success = probe("PGXL", args.pgxl_host, args.pgxl_port, code, args.pgxl_auth) and success
    return 0 if success else 1


if __name__ == "__main__":
    raise SystemExit(main())
