#!/usr/bin/env python3
"""HL2 discovery smoke test — HPSDR Protocol 1 (Metis) over UDP:1024.

Phase-0 spike for the aetherd Hl2Backend (see README.md). This is the very first
step: prove a Hermes-Lite 2 is reachable on the LAN and speaks Metis, before we
touch IQ streaming, tuning, or the IRadioBackend seam.

Protocol authority (Principle I — grep the spec, don't guess):
  - HPSDR "Metis - How it works" (discovery frame layout)
  - Hermes-Lite 2 wiki / gateware (board id 0x06, gateware version byte)

Discovery handshake:
  request  (63 B): 0xEF 0xFE 0x02  + 60 x 0x00, broadcast to <bcast>:1024
  reply    (60 B): 0xEF 0xFE <st>  MAC[6]  gwver  boardid  ...
                   st = 0x02 idle / 0x03 already streaming; HL2 boardid = 0x06

Run:  python3 discover.py [--bcast 255.255.255.255] [--timeout 2.0]
Needs no third-party deps. Read-only on the wire (discovery only — does NOT start
a stream, so it won't disturb another client already using the radio).
"""

import argparse
import socket
import sys

METIS_PORT = 1024
DISCOVERY_REQUEST = bytes([0xEF, 0xFE, 0x02]) + bytes(60)  # 63 bytes

BOARD_IDS = {
    0x00: "Metis",
    0x01: "Hermes",
    0x02: "Griffin",
    0x04: "Angelia",
    0x05: "Orion",
    0x06: "Hermes-Lite / Hermes-Lite 2",
    0x0A: "Orion mkII",
}


def parse_reply(data: bytes, addr) -> dict | None:
    """Parse a 60-byte Metis discovery reply. Returns None if it isn't one."""
    if len(data) < 11 or data[0] != 0xEF or data[1] != 0xFE:
        return None
    status = data[2]              # 0x02 idle, 0x03 sending data
    mac = ":".join(f"{b:02X}" for b in data[3:9])
    gwver = data[9]
    board = data[10]
    return {
        "ip": addr[0],
        "mac": mac,
        "status": "streaming" if status == 0x03 else "idle",
        "gateware": f"{gwver // 10}.{gwver % 10}",  # e.g. 0x49 -> "7.3"; refine vs HL2 wiki
        "gateware_raw": gwver,
        "board_id": board,
        "board": BOARD_IDS.get(board, f"unknown (0x{board:02X})"),
    }


def discover(bcast: str, timeout: float) -> list[dict]:
    with socket.socket(socket.AF_INET, socket.SOCK_DGRAM) as s:
        s.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
        s.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        s.bind(("", 0))
        s.settimeout(timeout)
        print(f"→ discovery broadcast to {bcast}:{METIS_PORT} "
              f"({len(DISCOVERY_REQUEST)} B), waiting {timeout}s ...")
        s.sendto(DISCOVERY_REQUEST, (bcast, METIS_PORT))

        found: dict[str, dict] = {}
        while True:
            try:
                data, addr = s.recvfrom(1024)
            except socket.timeout:
                break
            info = parse_reply(data, addr)
            if info:
                found[info["mac"]] = info  # de-dup by MAC
        return list(found.values())


def main() -> int:
    ap = argparse.ArgumentParser(description="HL2 / HPSDR Metis discovery")
    ap.add_argument("--bcast", default="255.255.255.255",
                    help="broadcast address (use your subnet bcast if 255... is filtered)")
    ap.add_argument("--timeout", type=float, default=2.0)
    args = ap.parse_args()

    radios = discover(args.bcast, args.timeout)
    if not radios:
        print("✗ no HPSDR/Hermes devices answered. Check: same L2 subnet, "
              "firewall on :1024, try --bcast <your subnet>.255.")
        return 1

    print(f"✓ {len(radios)} device(s):")
    for r in radios:
        print(f"  {r['ip']:<15}  {r['mac']}  {r['board']}  "
              f"gw={r['gateware']} (0x{r['gateware_raw']:02X})  [{r['status']}]")
    return 0


if __name__ == "__main__":
    sys.exit(main())
