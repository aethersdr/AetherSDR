#!/usr/bin/env python3
"""Safety regressions for tools/tx_meter_test.py; never connects or keys."""

import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import tx_meter_test as subject  # noqa: E402


def check(condition, message):
    if not condition:
        raise AssertionError(message)


class FakeTx:
    def __init__(self, meters, link_alive=True):
        self._meters = meters
        self._link_alive = link_alive
        self.stop_calls = 0
        self.unkey_calls = 0

    def meters(self):
        return self._meters

    def liveness(self):
        return {"connected": self._link_alive}

    def cmd(self, **_kwargs):
        self.stop_calls += 1
        return {"ok": True}

    def ensure_unkeyed(self):
        self.unkey_calls += 1
        return True


def meter_snapshot(fwd=5.0, fwd_age=0, swr=1.1, swr_age=0):
    all_meters = []
    if fwd_age >= 0:
        all_meters.append({
            "name": "FWDPWR", "unit": "dBm", "has_value": True,
            "value": fwd, "age_ms": fwd_age,
        })
    return {
        "fwdPower": fwd, "fwdPowerInstant": fwd,
        "fwdPowerAgeMs": fwd_age,
        "swr": swr,
        "swrAgeMs": swr_age,
        "all": all_meters,
    }


def run_once(fake, **kwargs):
    return subject.sample_window(fake, dur=0.05, settle=-1, **kwargs)


def test_over_watt_unkeys():
    fake = FakeTx(meter_snapshot(fwd=11.0))
    result = run_once(fake, max_watts=10.0)
    check("exceeds 10.0 W" in result["stopReason"], result)
    check(fake.stop_calls and fake.unkey_calls, "over-watt path did not unkey")


def test_high_swr_unkeys():
    fake = FakeTx(meter_snapshot(swr=3.0))
    result = run_once(fake, max_watts=10.0, max_swr=2.5)
    check("SWR 3.00" in result["stopReason"], result)
    check(fake.stop_calls and fake.unkey_calls, "high-SWR path did not unkey")


def test_missing_power_unkeys():
    old_deadline = subject.POWER_SAMPLE_DEADLINE_S
    subject.POWER_SAMPLE_DEADLINE_S = 0
    try:
        fake = FakeTx(meter_snapshot(fwd=0, fwd_age=-1))
        result = run_once(fake, max_watts=10.0)
    finally:
        subject.POWER_SAMPLE_DEADLINE_S = old_deadline
    check("no fresh calibrated FWDPWR" in result["stopReason"], result)
    check(fake.stop_calls and fake.unkey_calls, "missing-meter path did not unkey")


def test_link_loss_unkeys():
    fake = FakeTx(meter_snapshot(), link_alive=False)
    result = run_once(fake, max_watts=10.0)
    check(result["stopReason"] == "radio link is not alive", result)
    check(fake.stop_calls and fake.unkey_calls, "link-loss path did not unkey")


def test_antenna_discovery_is_exact():
    tree = {"roots": [{"children": [{
        "accessibleName": "TX antenna", "value": "ANT1",
    }]}]}
    check(subject.tx_antennas(tree) == {"ANT1"}, "ANT1 was not discovered")
    check(subject.tx_antennas({"roots": []}) == set(),
          "missing antenna must remain an empty, fail-closed set")


def test_unkey_uses_semantic_always_allowed_verb():
    class FakeBridge:
        def __init__(self):
            self.transmitting = True
            self.requests = []

        def request(self, request):
            self.requests.append(request)
            if request.get("cmd") == "get":
                prop = request.get("property")
                if prop == "tuning":
                    return {"value": False}
                if prop in ("mox", "transmitting"):
                    return {"value": self.transmitting}
                if request.get("model") == "radio" and prop == "transmitting":
                    return {"value": self.transmitting}
            if request.get("cmd") == "key" and request.get("value") == "off":
                self.transmitting = False
            return {"ok": True}

    bridge = FakeBridge()
    tx = subject.Tx(bridge)
    check(tx.ensure_unkeyed(), "semantic unkey did not reach safe state")
    check(any(r.get("cmd") == "key" and r.get("action") == "ptt"
              and r.get("value") == "off" for r in bridge.requests),
          "ensure_unkeyed did not use the always-allowed semantic key verb")



def test_unsmoothed_peak_not_hidden_by_display_average():
    meters = meter_snapshot(fwd=4)
    meters['fwdPowerInstant'] = 11
    fake = FakeTx(meters)
    result = run_once(fake, max_watts=10)
    check(result['peakFwdWatts'] == 11 and result['stopReason'], result)
    check(fake.unkey_calls, 'instantaneous peak did not unkey')


def test_context_disagreement_unkeys():
    fake = FakeTx(meter_snapshot())
    result = run_once(fake, max_watts=10, guard=lambda: 'TX frequency changed')
    check(result['stopReason'] == 'TX frequency changed' and fake.unkey_calls, result)


def test_unknown_link_unkeys():
    fake = FakeTx(meter_snapshot(), link_alive=None)
    result = run_once(fake, max_watts=10)
    check(result['stopReason'] == 'radio link is not alive' and fake.unkey_calls, result)


def test_missing_swr_unkeys():
    deadline = subject.POWER_SAMPLE_DEADLINE_S
    subject.POWER_SAMPLE_DEADLINE_S = 0
    try:
        fake = FakeTx(meter_snapshot(swr=None, swr_age=-1))
        result = run_once(fake, max_watts=10)
    finally:
        subject.POWER_SAMPLE_DEADLINE_S = deadline
    check(result['stopReason'] == 'no fresh SWR sample before safety deadline' and fake.unkey_calls, result)


def test_authorized_context_refuses_offsets_and_wrong_port():
    class ContextTx:
        def __init__(self):
            self.states = {
                'radio': {'connected': True, 'serial': 'test'},
                'transmit': {'voxEnable': False, 'atuStatus': 'manual_bypass', 'rfPower': 5, 'tunePower': 2},
                'slice': {'txSlice': True, 'txAntenna': 'ANT2', 'frequency': 14.2, 'mode': 'USB',
                          'xitOn': False, 'txOffsetFreq': 0, 'repeaterOffsetDir': 'simplex'}}
        def g(self, model, **kwargs): return self.states[model]
        def cmd(self, **kwargs):
            return {'txAllowed': True} if kwargs.get('cmd') == 'whoami' else {'roots': [{'accessibleName': 'TX antenna', 'value': 'ANT2'}]}
    tx = ContextTx()
    gate = lambda: subject.authorized_context(tx, 'test', 'ANT2', 14.2, 'USB', 5, widgets=True)
    check(gate() is None, gate())
    for field, value in [('txAntenna','ANT1'), ('frequency',14.201), ('mode','CW'), ('xitOn',True), ('txOffsetFreq',.1)]:
        old = tx.states['slice'][field]; tx.states['slice'][field] = value
        check(gate() is not None, field + ' was accepted')
        tx.states['slice'][field] = old
    tx.states['transmit']['rfPower'] = float('nan')
    check(gate() is not None, 'NaN control was accepted')

def test_unknown_transmit_flags_never_confirm_unkeyed():
    from unittest.mock import patch
    class UnknownBridge:
        def __init__(self): self.commands = []
        def request(self, request):
            self.commands.append(request)
            return {"ok": True}  # No model flags: a successful envelope is not TX-off evidence.
    bridge = UnknownBridge()
    with patch.object(subject.time, "sleep"):
        check(subject.Tx(bridge).ensure_unkeyed() is False, "unknown TX flags were treated as off")
    check(any(x.get("cmd") == "key" and x.get("value") == "off" for x in bridge.commands),
          "missing flags did not attempt semantic unkey")


if __name__ == "__main__":
    test_over_watt_unkeys()
    test_high_swr_unkeys()
    test_missing_power_unkeys()
    test_link_loss_unkeys()
    test_antenna_discovery_is_exact()
    test_unkey_uses_semantic_always_allowed_verb()
    test_unsmoothed_peak_not_hidden_by_display_average()
    test_context_disagreement_unkeys()
    test_unknown_link_unkeys()
    test_missing_swr_unkeys()
    test_authorized_context_refuses_offsets_and_wrong_port()
    test_unknown_transmit_flags_never_confirm_unkeyed()
    print("TX meter safety checks passed")
