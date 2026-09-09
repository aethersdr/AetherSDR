#!/usr/bin/env python3
"""Socket-free runner policy/evidence tests. Fixtures are data, not radio peers."""
import copy
import json
from pathlib import Path
import tempfile
from radiocert_persist_applets import AppletRun, Control, ALL_CONTROLS, flatten, widgets, widget_value, initial_settling
import unittest
from unittest.mock import Mock, patch

from radiocert_persist import (Journal, Run, StopRun, Supervisor, alternate,
                               atomic_json, band_of, compare, ready, values, show_control, named_widget)


def snapshot():
    return {"schemaVersion": 1, "identity": {"txAllowed": False, "readOnly": False},
            "family": "flex", "clientSettingsDomains": 0, "radio": {"connected": True, "serial": "test", "transmitting": False},
            "transmit": {"transmitting": False, "tuning": False, "mox": False, "voxEnable": False},
            "clients": {"clients": [{"isUs": True}]},
            "slices": [{"sliceId": 3, "panId": "0x42", "active": True, "linkedTo": -1,
                        "frequency": 14.2, "mode": "USB", "filterLow": 250, "filterHigh": 2750,
                        "rxAntenna": "ANT1", "txAntenna": "ANT2", "audioMute": False}],
            "pans": [{"panId": "0x42", "ownedByUs": True, "clientHandle": "0x123",
                      "centerKnown": True, "average": 0, "fps": 25,
                      "rxAntenna": "ANT1", "antennas": ["ANT1", "ANT2"]}],
            "display": {"pans": [{"panId": "0x42", "fftAverage": 0, "fftFps": 25,
                                    "showGrid": False, "wfColorScheme": 0}]}}


class EvidenceTest(unittest.TestCase):
    def test_zero_and_false_are_positive_evidence(self):
        self.assertEqual(compare({"a": 0, "b": False}, [{"a": 0, "b": False}])["outcome"], "ESTABLISHED")

    def test_missing_false_never_passes(self):
        self.assertEqual(compare({"a": False}, [{}])["outcome"], "INCONCLUSIVE")
        self.assertEqual(compare({"a": 0}, [{"a": None}])["outcome"], "INCONCLUSIVE")

    def test_bool_is_not_integer(self):
        self.assertEqual(compare({"a": False}, [{"a": 0}])["outcome"], "CONCERN")

    def test_no_vacuous_success(self):
        self.assertEqual(compare({}, [{}])["outcome"], "INCONCLUSIVE")
        self.assertEqual(compare({"x": 1}, [])["outcome"], "INCONCLUSIVE")

    def test_delayed_overwrite_survives_later_recovery(self):
        result = compare({"a": 17}, [{"a": 17}, {"a": 0}, {"a": 17}])
        self.assertEqual(result["outcome"], "CONCERN")
        self.assertEqual(result["differences"][0]["sample"], 1)

    def test_concern_not_hidden_by_missing_other_field(self):
        self.assertEqual(compare({"a": 2, "b": 3}, [{"a": 1}])["outcome"], "CONCERN")

    def test_new_pan_id_maps_by_relationship_not_old_index(self):
        data = snapshot()
        self.assertEqual(values(data, "test")["display.fftAverage"], 0)
        for group in (data["slices"], data["pans"], data["display"]["pans"]):
            group[0]["panId"] = "0x99"
        self.assertEqual(values(data, "test")["display.fftAverage"], 0)
        data["display"]["pans"][0]["panId"] = "0x42"
        with self.assertRaises(StopRun):
            values(data, "test")

    def test_unlinked_sentinel_is_valid_but_slice_zero_link_is_not(self):
        data = snapshot()
        ready(data, "test")
        data["slices"][0]["linkedTo"] = 0
        with self.assertRaises(StopRun):
            ready(data, "test")

    def test_preflight_refuses_unknown_or_unsafe_state(self):
        mutations = [lambda d: d["identity"].update(txAllowed=True),
                     lambda d: d["radio"].update(transmitting=True),
                     lambda d: d["transmit"].update(voxEnable=True),
                     lambda d: d["transmit"].pop("voxEnable"),
                     lambda d: d["transmit"].update(tuning=True),
                     lambda d: d["radio"].pop("transmitting"),
                     lambda d: d["radio"].update(serial="other"),
                     lambda d: d["pans"][0].update(clientHandle=""),
                     lambda d: d["pans"][0].update(average=-1),
                     lambda d: d["pans"].append(copy.deepcopy(d["pans"][0])),
                     lambda d: d["clients"]["clients"].append({"isUs": False}),
                     lambda d: d["slices"][0].update(locked=True),
                     lambda d: d.update(family="icom")]
        for mutation in mutations:
            data = snapshot()
            mutation(data)
            with self.subTest(data=data), self.assertRaises(StopRun):
                ready(data, "test")

    def test_hidden_display_panel_is_opened_before_control(self):
        hidden = {"children": [{"objectName": "slider", "visible": False, "enabled": True},
                               {"objectName": "panMenuDisplayBtn", "visible": True}]}
        shown = copy.deepcopy(hidden)
        shown["children"][0]["visible"] = True
        read = Mock(side_effect=[hidden, shown])
        act = Mock()
        show_control(read, act, "slider")
        self.assertEqual(act.call_args_list[0].args[0],
                         {"cmd": "invoke", "target": "panMenuDisplayBtn", "action": "click"})
        self.assertEqual(act.call_args_list[1].args[0]["cmd"], "scrollTo")

    def test_ambiguous_widget_is_not_driven(self):
        with self.assertRaises(StopRun):
            named_widget({"children": [{"objectName": "slider"}, {"objectName": "slider"}]}, "slider")

    def test_unknown_band_is_not_guessed(self):
        for value in (None, float("nan"), float("inf"), 50.1):
            self.assertIsNone(band_of(value))
        self.assertEqual(band_of(14.2), "20m")
        self.assertEqual(band_of(7.1), "40m")

    def test_seed_always_differs(self):
        self.assertEqual(alternate(17, 17, 19), 19)
        self.assertEqual(alternate(0, 17, 19), 17)



class AppletPolicyTest(unittest.TestCase):
    def test_nested_eq_zero_and_false_survive_flattening(self):
        self.assertEqual(flatten({'eq': {'rx': {'63': 0}, 'rxEnabled': False}}),
                         {'eq.rx.63': 0, 'eq.rxEnabled': False})

    def test_scoped_widget_does_not_select_another_applet(self):
        tree = {'roots': [{'class': 'AetherSDR::RxApplet', 'children': [
            {'accessibleName': 'AF gain', 'value': 23}]},
            {'class': 'PanadapterApplet', 'children': [{'accessibleName': 'AF gain', 'value': 99}]}]}
        self.assertEqual(widgets(tree, 'RxApplet/AF gain')[0]['value'], 23)
        self.assertEqual(len(widgets(tree, 'AF gain')), 2)

    def test_forged_keying_control_is_refused_before_action(self):
        run = Mock(journal=Mock(data={}))
        applets = AppletRun(run)
        with self.assertRaises(ValueError):
            applets.set_control(Control('transmit.voxEnable', 'VOX voice-operated transmit', True, False,
                                        'setChecked'), True, 'not authorized')
        run.action.assert_not_called()

    def test_numeric_widget_text_is_typed_without_defaulting_missing(self):
        self.assertEqual(widget_value({'range': {'min': 0}, 'value': '0'}, 'setValue'), 0)
        self.assertIsNone(widget_value({'range': {'min': 0}}, 'setValue'))
        self.assertEqual(widget_value({'value': 'Fast'}, 'setCurrentText'), 'Fast')

    def test_only_leading_baseline_is_initial_settling(self):
        expected = {'mute': True, 'gain': 23}
        samples = [{'mute': False, 'gain': 23}, {'mute': True, 'gain': 23}]
        self.assertEqual(initial_settling(expected, samples, 'mute', False), 1)
        samples.append({'mute': False, 'gain': 23})
        start = initial_settling(expected, samples, 'mute', False)
        self.assertEqual(compare(expected, samples[start:])['outcome'], 'CONCERN')
        samples[0]['gain'] = 50
        self.assertEqual(initial_settling(expected, samples, 'mute', False), 0)
        self.assertEqual(initial_settling({'a': 17}, [{'a': 0}, {'a': 17}], 'a', 50), 0)
        self.assertEqual(initial_settling({'a': False}, [{'a': 0}, {'a': False}], 'a', True), 0)

    def test_catalogue_has_distinct_rx_tx_eq_and_no_arming_actions(self):
        fields = [c.field for c in ALL_CONTROLS]
        self.assertEqual(len(fields), len(set(fields)))
        self.assertEqual(len([f for f in fields if f.startswith('equalizer.rx.')]), 8)
        self.assertEqual(len([f for f in fields if f.startswith('equalizer.tx.')]), 8)
        self.assertNotIn('transmit.voxEnable', fields)
        self.assertNotIn('transmit.cwBreakIn', fields)

class AppletEvidenceTest(unittest.TestCase):
    def exercise(self, values, *, seed=False, prior=None, hidden=False):
        directory = tempfile.TemporaryDirectory()
        self.addCleanup(directory.cleanup)
        journal = Journal(Path(directory.name) / 'persist.json', {})
        run = Mock(journal=journal)
        applets = AppletRun(run)
        applets.expected = {'slice.audioGain': 23}
        applets.snapshot = Mock(return_value={'slices': [{'audioGain': 23}]})
        if prior:
            applets.coverage['slice.audioGain']['seedOutcome'] = prior
        def tree(value):
            return {'roots': [{'class': 'RxApplet', 'children': [{
                'accessibleName': 'AF gain', 'value': str(value) if value is not None else None,
                'range': {}, 'visible': not hidden, 'enabled': True}]}]}
        run.supervisor.request.side_effect = [tree(value) for value in values]
        name = 'set slice.audioGain' if seed else 'full client restart'
        with patch('radiocert_persist_applets.time.sleep'):
            result = applets.observe(name, samples=len(values))
        journal.report()
        return result, applets.coverage['slice.audioGain'], journal

    def test_widget_mismatch_downgrades_result_and_markdown(self):
        result, coverage, journal = self.exercise([99])
        self.assertEqual(result['outcome'], 'CONCERN')
        self.assertEqual(coverage['transitions']['full client restart'], 'CONCERN')
        report = journal.path.with_suffix('.md').read_text()
        self.assertIn('| applets: full client restart | CONCERN |', report)
        self.assertIn('| slice.audioGain | False | CONCERN |', report)

    def test_widget_recovery_does_not_hide_earlier_failure(self):
        result, _, _ = self.exercise([23, 99, 23])
        self.assertEqual(result['outcome'], 'CONCERN')
        self.assertEqual(result['differences'][0]['sample'], 1)

    def test_missing_widget_value_stays_inconclusive(self):
        result, coverage, _ = self.exercise([None])
        self.assertEqual(result['outcome'], 'INCONCLUSIVE')
        self.assertEqual(coverage['transitions']['full client restart'], 'INCONCLUSIVE')

    def test_hidden_widget_is_a_gap_without_selecting_or_repairing_it(self):
        result, coverage, _ = self.exercise([23], hidden=True)
        self.assertEqual(result['outcome'], 'INCONCLUSIVE')
        self.assertEqual(coverage['widgetTransitions']['full client restart']['outcome'], 'INCONCLUSIVE')

    def test_seed_widget_failure_prevents_later_retention_success(self):
        _, seed, _ = self.exercise([99, 23], seed=True)
        outcome = seed['transitions']['set slice.audioGain']
        self.assertEqual(outcome, 'CONCERN')
        result, coverage, _ = self.exercise([23], prior=outcome)
        self.assertEqual(result['outcome'], 'INCONCLUSIVE')
        self.assertEqual(coverage['transitions']['full client restart'], 'INCONCLUSIVE')

    def test_matching_model_and_widget_establishes_observation(self):
        result, _, _ = self.exercise([23, 23])
        self.assertEqual(result['outcome'], 'ESTABLISHED')

class JournalTest(unittest.TestCase):
    def test_markdown_report_is_written(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = Journal(Path(directory) / "persist.json", {})
            journal.result("restart", {"a": False}, [{"a": False}])
            journal.data["status"] = "completed"
            journal.report()
            report = (Path(directory) / "persist.md").read_text()
            self.assertIn("| restart | ESTABLISHED |", report)
            self.assertIn("independent wire readback", report)

    def test_profile_initialization_is_before_gui_and_reads_back(self):
        supervisor = Supervisor(Path("app"), Path("profile"), Path("output"), Mock())
        with patch("radiocert_persist.subprocess.run") as command:
            command.side_effect = [Mock(returncode=0), Mock(returncode=0, stdout="False\n")]
            supervisor.initialize_profile()
            self.assertEqual(command.call_args_list[0].args[0][1:],
                             ["--config", "set", "AutoConnectToLastRadio", "False"])
            self.assertEqual(command.call_args_list[1].args[0][1:],
                             ["--config", "get", "AutoConnectToLastRadio"])

    def test_seed_concern_prevents_later_retention_claim(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = Journal(Path(directory) / "persist.json", {})
            result = journal.result("restart", {"a": 17}, [{"a": 17}], {"20m": ["a"]})
            self.assertEqual(result["outcome"], "INCONCLUSIVE")
            self.assertEqual(result["observationOutcome"], "ESTABLISHED")
            self.assertEqual(journal.data["results"][-1]["outcome"], "INCONCLUSIVE")

    def test_atomic_failure_keeps_prior_document(self):
        with tempfile.TemporaryDirectory() as directory:
            path = Path(directory) / "report.json"
            atomic_json(path, {"old": True})
            with patch("radiocert_persist.os.replace", side_effect=OSError("disk")):
                with self.assertRaises(OSError):
                    atomic_json(path, {"new": True})
            self.assertEqual(json.loads(path.read_text()), {"old": True})
            self.assertEqual(list(Path(directory).iterdir()), [path])

    def test_pending_intent_durable_before_ambiguous_send(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = Journal(Path(directory) / "report.json", {})
            supervisor = Mock()
            supervisor.request.side_effect = [snapshot(), ConnectionError("lost reply")]
            run = Run(supervisor, "test", journal)
            with self.assertRaises(ConnectionError):
                run.action({"cmd": "slice", "action": "mode", "value": "USB"}, "test")
            durable = json.loads(journal.path.read_text())
            self.assertEqual(durable["events"][-1]["kind"], "action-pending")
            self.assertEqual(durable["events"][-1]["before"]["radio"]["serial"], "test")

    def test_unsafe_preflight_sends_no_mutation(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = Journal(Path(directory) / "report.json", {})
            supervisor = Mock()
            data = snapshot()
            data["radio"]["transmitting"] = True
            supervisor.request.return_value = data
            run = Run(supervisor, "test", journal)
            with self.assertRaises(StopRun):
                run.action({"cmd": "slice", "action": "mode", "value": "USB"}, "test")
            self.assertEqual(supervisor.request.call_count, 1)
            self.assertFalse(any(e["kind"] == "action-pending" for e in journal.data["events"]))

    def test_cleanup_conflict_does_not_overwrite(self):
        with tempfile.TemporaryDirectory() as directory:
            journal = Journal(Path(directory) / "report.json", {})
            run = Run(Mock(), "test", journal)
            data = snapshot()
            run.baselines = {"20m": data}
            run.expected = {"20m": {"slice.mode": "LSB"}}
            run.band = Mock(return_value=data)
            run.action = Mock()
            run.cleanup()
            run.action.assert_not_called()
            self.assertEqual(journal.data["cleanup"][0]["outcome"], "INCONCLUSIVE")

    def test_rx_antenna_uses_only_receive_setter(self):
        run = Run(Mock(), "test", Mock(data={}), ("ANT1", "ANT2"))
        run.snapshot = Mock(return_value=snapshot())
        run.action = Mock()
        run.rx_antenna("ANT2")
        self.assertEqual(run.action.call_args.args[0], {"cmd": "slice", "action": "rxant", "value": "ANT2"})
        with self.assertRaises(StopRun):
            run.rx_antenna("RX_A")
        self.assertEqual(run.action.call_count, 1)

    def test_missing_antenna_capability_blocks_write(self):
        run = Run(Mock(), "test", Mock(data={}), ("ANT1", "ANT2"))
        data = snapshot()
        data["pans"][0]["antennas"] = ["ANT1"]
        run.snapshot = Mock(return_value=data)
        run.action = Mock()
        with self.assertRaises(StopRun):
            run.rx_antenna("ANT2")
        run.action.assert_not_called()

    def test_refuse_second_live_process(self):
        supervisor = Supervisor(Path("app"), Path("profile"), Path("output"), Mock())
        supervisor.process = Mock()
        supervisor.process.poll.return_value = None
        with patch("radiocert_persist.subprocess.Popen") as launch:
            with self.assertRaises(StopRun):
                supervisor.launch()
            launch.assert_not_called()

    def test_quit_uses_normal_action_without_forced_save(self):
        supervisor = Supervisor(Path("app"), Path("profile"), Path("output"), Mock())
        supervisor.process, supervisor.bridge = Mock(), Mock()
        supervisor.process.wait.return_value = 0
        supervisor.bridge.request.return_value = {"ok": True}
        bridge = supervisor.bridge
        supervisor.quit()
        bridge.request.assert_called_once_with({"cmd": "invoke", "target": "Quit", "action": "trigger"}, timeout_seconds=10)
        supervisor.process.wait.assert_called_once_with(timeout=30)



class MultiSlicePolicyTest(unittest.TestCase):
    def two_slices(self):
        data = snapshot()
        data['radio'].update(maxSlices=2, slots=[{'id': 3, 'state': 'ours'}, {'id': 7, 'state': 'ours'}])
        data['slices'][0]['letter'] = 'A'
        second = copy.deepcopy(data['slices'][0])
        second.update(sliceId=7, letter='B', active=False)
        data['slices'].append(second)
        return data

    def test_opt_in_keeps_single_slice_default(self):
        data = self.two_slices()
        with self.assertRaises(StopRun):
            ready(data, 'test')
        self.assertEqual(ready(data, 'test', slice_count=2)[0]['letter'], 'A')

    def test_every_slice_requires_identity_ownership_and_safe_topology(self):
        mutations = [lambda d: d['slices'][1].update(sliceId=3),
                     lambda d: d['slices'][1].update(letter='A'),
                     lambda d: d['slices'][1].pop('letter'),
                     lambda d: d['slices'][1].update(active=True),
                     lambda d: d['slices'][1].update(panId='foreign'),
                     lambda d: d['slices'][1].update(locked=True),
                     lambda d: d['slices'][1].update(linkedTo=3),
                     lambda d: d['radio']['slots'][1].update(state='foreign'),
                     lambda d: d['radio'].update(maxSlices=1)]
        for mutation in mutations:
            data = self.two_slices(); mutation(data)
            with self.subTest(data=data), self.assertRaises(StopRun):
                ready(data, 'test', slice_count=2)

    def test_restart_rebinds_letters_not_numeric_ids(self):
        from radiocert_persist_multislice import by_letter
        data = self.two_slices()
        data['slices'].reverse()
        data['slices'][0]['sliceId'] = 42
        self.assertEqual(by_letter(data)['A']['sliceId'], 3)
        self.assertEqual(by_letter(data)['B']['sliceId'], 42)

    def test_restoration_refuses_changed_or_missing_values(self):
        from radiocert_persist_multislice import restoration_conflicts
        self.assertEqual(restoration_conflicts({'squelchLevel': 26}, {'squelchLevel': 26})['outcome'], 'ESTABLISHED')
        self.assertEqual(restoration_conflicts({'squelchLevel': 39}, {'squelchLevel': 26})['outcome'], 'CONCERN')
        self.assertEqual(restoration_conflicts({}, {'squelchLevel': 26})['outcome'], 'INCONCLUSIVE')

    def test_never_remove_original_slice(self):
        from radiocert_persist_multislice import MultiSliceRun
        with tempfile.TemporaryDirectory() as directory:
            run = MultiSliceRun(Mock(), 'test', Journal(Path(directory)/'j.json', {}))
            run.original = run.extra = 'A'
            run.action = Mock()
            with self.assertRaises(StopRun):
                run.remove_extra()
            run.action.assert_not_called()

    def test_peer_state_leakage_is_retained(self):
        result = compare({'A.sql': 26, 'B.sql': 39},
                         [{'A.sql': 26, 'B.sql': 39}, {'A.sql': 39, 'B.sql': 39}, {'A.sql': 26, 'B.sql': 39}])
        self.assertEqual(result['outcome'], 'CONCERN')

    def test_pan_restoration_refuses_newer_center(self):
        from radiocert_persist_multislice import MultiSliceRun
        with tempfile.TemporaryDirectory() as directory:
            run = MultiSliceRun(Mock(), 'test', Journal(Path(directory)/'j.json', {}))
            run.pan_expected = {'centerMhz': 14.12}
            run.wait_ready = Mock(return_value={'pans': [{'centerMhz': 14.15}]})
            run.action = Mock()
            with self.assertRaises(StopRun):
                run.restore_pan({'centerMhz': 14.1})
            run.action.assert_not_called()

    def test_pan_center_compares_at_flex_wire_resolution(self):
        from radiocert_persist_multislice import pan_state
        self.assertEqual(pan_state({'centerMhz':14.126199999999999}), pan_state({'centerMhz':14.1262}))
        self.assertNotEqual(pan_state({'centerMhz':14.126201}), pan_state({'centerMhz':14.1262}))

    def test_explicit_tx_permission_still_requires_unkeyed_state(self):
        data = snapshot();data['identity']['txAllowed'] = True
        with self.assertRaises(StopRun):ready(data, 'test')
        ready(data, 'test', tx_permission=True)
        data['transmit']['tuning'] = True
        with self.assertRaises(StopRun):ready(data, 'test', tx_permission=True)

class FmAgcPolicyTest(unittest.TestCase):
    def states(self):
        return {'A': {'mode': 'USB', 'agcMode': 'fast', 'agcThreshold': 43,
                      'flexAgcOffLevel': 17, 'filterLow': 150, 'squelch': True},
                'B': {'mode': 'LSB', 'agcMode': 'slow', 'agcThreshold': 61,
                      'flexAgcOffLevel': 29, 'filterLow': -2650, 'squelch': False}}

    def test_slider_selects_off_level_only_when_agc_is_off(self):
        from radiocert_persist_multislice import agc_slider_field
        state = self.states()['A']
        self.assertEqual(state[agc_slider_field(state)], 43)
        state['agcMode'] = 'off'
        self.assertEqual(state[agc_slider_field(state)], 17)

    def test_off_level_is_part_of_retention_and_restoration(self):
        from radiocert_persist_multislice import slice_state, restoration_conflicts
        state = self.states()['A']
        self.assertEqual(slice_state(state)['flexAgcOffLevel'], 17)
        self.assertEqual(restoration_conflicts({**state, 'flexAgcOffLevel': 50}, state)['outcome'], 'CONCERN')
        del state['flexAgcOffLevel']
        self.assertEqual(restoration_conflicts(state, self.states()['A'])['outcome'], 'INCONCLUSIVE')

    def test_stable_fm_reset_is_cleanup_input_not_retention_success(self):
        from radiocert_persist_multislice import fm_cleanup_state
        before = self.states(); after = copy.deepcopy(before)
        after['A'].update(agcMode='med', agcThreshold=50, flexAgcOffLevel=50)
        with tempfile.TemporaryDirectory() as directory:
            journal = Journal(Path(directory)/'j.json', {})
            journal.result('FM retention', flatten(before), [flatten(after)])
            self.assertEqual(fm_cleanup_state(before, [after, after], 'A'), after)
            journal.result('cleanup', flatten(before), [flatten(before)])
            self.assertEqual(journal.data['results'][0]['outcome'], 'CONCERN')

    def test_cleanup_refuses_missing_mistyped_peer_unrelated_and_unstable_changes(self):
        from radiocert_persist_multislice import fm_cleanup_state
        before = self.states()
        mutations = [lambda d: d['A'].pop('flexAgcOffLevel'),
                     lambda d: d['A'].update(flexAgcOffLevel=True),
                     lambda d: d['B'].update(flexAgcOffLevel=50),
                     lambda d: d['A'].update(squelch=False),
                     lambda d: d.pop('B'),
                     lambda d: d['A'].update(agcMode='unknown'),
                     lambda d: d['A'].update(flexAgcOffLevel=101)]
        for mutate in mutations:
            after = copy.deepcopy(before); mutate(after)
            with self.subTest(after=after), self.assertRaises(StopRun):
                fm_cleanup_state(before, [after, after], 'A')
        changed = copy.deepcopy(before); changed['A']['flexAgcOffLevel'] = 50
        with self.assertRaises(StopRun):
            fm_cleanup_state(before, [changed, before], 'A')

    def test_set_state_sets_both_slider_meanings_even_with_final_agc_off(self):
        from radiocert_persist_multislice import MultiSliceRun
        with tempfile.TemporaryDirectory() as directory:
            run = MultiSliceRun(Mock(), 'test', Journal(Path(directory)/'j.json', {}))
            run.select = Mock(); run.action = Mock(); run.wait_ready = Mock()
            run.widget = Mock(); run.sql = Mock()
            state = {**self.states()['A'], 'frequency': 14.18, 'filterHigh': 2450,
                     'audioGain': 23, 'audioPan': 25, 'audioMute': False, 'squelchLevel': 26,
                     'agcMode': 'off'}
            run.set_state('A', state)
            self.assertEqual([c.args for c in run.widget.call_args_list[:5]],
                             [('agcMode', 'med'), ('agcThreshold', 43), ('agcMode', 'off'),
                              ('agcThreshold', 17), ('agcMode', 'off')])

    def test_fm_only_sends_mode_until_retention_is_recorded(self):
        from radiocert_persist_multislice import MultiSliceRun
        with tempfile.TemporaryDirectory() as directory:
            run = MultiSliceRun(Mock(), 'test', Journal(Path(directory)/'j.json', {}))
            run.expected = self.states(); run.select = Mock(); run.action = Mock()
            run.wait_ready = Mock(); run.snapshot = Mock(return_value={})
            run.supervisor.request.return_value = {}
            run.assert_ready = Mock()
            run.set_state = Mock()
            def observe(name, *args):
                if name.startswith('FM return'):
                    raise StopRun('stop at retention boundary')
                return {'outcome': 'ESTABLISHED'}
            run.observe = Mock(side_effect=observe)
            with patch('radiocert_persist_multislice.time.sleep'), self.assertRaises(StopRun):
                run.fm_roundtrip('A')
            self.assertEqual([c.args[0] for c in run.action.call_args_list],
                             [{'cmd': 'slice', 'action': 'mode', 'value': 'FM'},
                              {'cmd': 'slice', 'action': 'mode', 'value': 'USB'}])
            run.set_state.assert_not_called()

if __name__ == "__main__":
    unittest.main()
