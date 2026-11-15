#!/usr/bin/env python3
"""Check GPS model/readout propagation on an isolated, disconnected bridge app.

Usage: python3 tools/automation_gps_fix_probe.py --socket SOCKET --output DIR
The app must already be running with the agent automation bridge enabled.
Fixtures use a public observatory location; this does not prove GPS acquisition.
"""
import argparse
import json
from pathlib import Path
import time

from automation_probe import Bridge, discover_socket


def nodes(value):
    if isinstance(value, dict):
        yield value
        for child in value.values():
            yield from nodes(child)
    elif isinstance(value, list):
        for child in value:
            yield from nodes(child)


def status_bar_clock(tree):
    return next(n for n in nodes(tree) if n.get("accessibleName") == "UTC time")


def run(bridge, output):
    output.mkdir(parents=True, exist_ok=True)
    evidence = []

    def request(**args):
        result = bridge.request(args, timeout_seconds=15)
        if not result.get("ok"):
            raise AssertionError(f"{args}: {result}")
        return result

    radio = request(cmd="get", model="radio")["radio"]
    assert not radio["connected"] and not radio["transmitting"], "disconnect first"
    request(cmd="gps", action="fixture", value="8000")
    try:
        request(cmd="invoke", target="AetherGPS...", action="trigger")
        time.sleep(0.3)
        for token, display, valid in [
            ("UNKNOWN", "Unknown", True),
            ("3D", "3D", True),
            ("2D", "2D", True),
            ("TIME_ONLY", "Time only", False),
            ("NO_FIX", "No fix", False),
        ]:
            request(cmd="gps", action="fixfixture", value=token)
            gps = request(cmd="get", model="gps")["gps"]
            expected = "Unknown" if token == "UNKNOWN" else token
            assert gps["fixState"] == expected, gps
            assert gps["positionValid"] is valid, gps
            assert gps["timeValid"] is (token != "NO_FIX"), gps
            assert gps["status"] == "Locked" and gps["referenceLocked"] is True, gps
            tree = request(cmd="dumpTree")
            readout = next(n for n in nodes(tree) if n.get("objectName") == "gpsFixStatus")
            assert readout["value"] == display, readout
            assert "Receiver status: Locked" in readout.get("toolTip", ""), readout
            labels = {n.get("objectName"): n for n in nodes(tree) if n.get("objectName")}
            assert (labels["gpsLatitude"]["value"] != "—") is valid, labels["gpsLatitude"]
            assert (labels["gpsLongitude"]["value"] != "—") is valid, labels["gpsLongitude"]
            if token == "TIME_ONLY":
                assert not labels["gpsClockAgreement"]["value"].startswith("Unavailable"), labels["gpsClockAgreement"]
                assert status_bar_clock(tree)["value"] == gps["utcTime"], status_bar_clock(tree)
            if token == "NO_FIX":
                # A fix-only update preserves the cached GPS time and reference
                # lock. The status-bar clock must nevertheless resume ticking.
                time.sleep(1.2)
                first_tree = request(cmd="dumpTree")
                first = status_bar_clock(first_tree)["value"]
                time.sleep(1.2)
                second_tree = request(cmd="dumpTree")
                second = status_bar_clock(second_tree)["value"]
                assert first != second, ("NO_FIX status-bar clock is frozen", first, second)
                assert request(cmd="get", model="gps")["gps"]["utcTime"] == gps["utcTime"]
                evidence.append({"fixture": "NO_FIX status-bar fallback", "clockSamples": [first, second]})
            evidence.append({"fixture": token, "gps": gps, "readout": readout})
            request(cmd="grab", target="GpsLocationDialog", path=str(output / f"{token.lower()}.png"))
        # Returning to the same position after either validity veto must resume
        # address lookup. A previous query must not leave the waiting UI latched.
        for lost_fix in ["TIME_ONLY", "NO_FIX"]:
            request(cmd="gps", action="fixfixture", value=lost_fix)
            request(cmd="gps", action="fixfixture", value="3D")
            tree = request(cmd="dumpTree")
            labels = {n.get("objectName"): n for n in nodes(tree) if n.get("objectName")}
            address = labels["gpsMappedAddress"]["value"]
            assert address != "Waiting for a valid GPS fix", (lost_fix, address)
            evidence.append({"fixture": f"3D after {lost_fix}", "address": address,
                             "refreshEnabled": labels["gpsRefreshAddress"]["enabled"]})
        # Fixture cleanup must remove both fix and legacy validity. A fix label
        # alone after cleanup cannot make the position valid again.
        request(cmd="gps", action="clearfixture")
        request(cmd="gps", action="fixfixture", value="3D")
        gps = request(cmd="get", model="gps")["gps"]
        assert gps["fixState"] == "3D" and gps["positionValid"] is False, gps
        assert gps["available"] is True and gps["referenceLocked"] is False, gps
        tree = request(cmd="dumpTree")
        readout = next(n for n in nodes(tree) if n.get("objectName") == "gpsFixStatus")
        assert readout["value"] == "3D", readout
        evidence.append({"fixture": "3D without position report", "gps": gps})
        request(cmd="gps", action="fixfixture", value="TIME_ONLY")
        gps = request(cmd="get", model="gps")["gps"]
        assert not gps["positionValid"] and not gps["timeValid"] and not gps["referenceLocked"], gps
        tree = request(cmd="dumpTree")
        readout = next(n for n in nodes(tree) if n.get("objectName") == "gpsFixStatus")
        assert readout["value"] == "Time only", readout
        assert "Time validity and frequency-reference lock are shown separately." in readout["accessibleDescription"], readout
        assert "are available" not in readout["accessibleDescription"], readout
        evidence.append({"fixture": "TIME_ONLY without validity or reference lock", "gps": gps, "readout": readout})
        radio = request(cmd="get", model="radio")["radio"]
        assert not radio["connected"] and not radio["transmitting"], radio
    finally:
        request(cmd="gps", action="clearfixture")
        (output / "gps-fix-proof.json").write_text(json.dumps(evidence, indent=2) + "\n")
    print(f"PASS: five fix states, real AetherGPS readout, independent lock and validity; {output}")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--socket", default=discover_socket())
    parser.add_argument("--output", type=Path, required=True)
    args = parser.parse_args()
    if not args.socket:
        parser.error("no bridge socket found")
    bridge = Bridge(args.socket)
    try:
        run(bridge, args.output.resolve())
    finally:
        bridge.close()


if __name__ == "__main__":
    main()
