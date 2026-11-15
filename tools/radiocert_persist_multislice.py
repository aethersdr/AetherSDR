#!/usr/bin/env python3
"""RX-only two-slice Persist scenario; supervised restart and guarded restoration.

Starts with one owned slice/pan, creates only the additional test slice, and
never removes the operator's original slice. No TX permission is inherited.
"""
import argparse
import hashlib
import os
import subprocess
import sys
import time
from pathlib import Path

from radiocert_persist import Journal, Run, StopRun, Supervisor, compare, ready
from radiocert_persist_applets import widgets, widget_value

FIELDS = ('frequency', 'mode', 'filterLow', 'filterHigh', 'agcMode', 'agcThreshold', 'flexAgcOffLevel',
          'audioGain', 'audioPan', 'audioMute', 'squelch', 'squelchLevel', 'manualSquelchLevel')
SENTINELS = ('rxAntenna', 'txAntenna', 'txSlice', 'ritOn', 'ritFreq', 'xitOn', 'xitFreq')
PAN_FIELDS = ('centerMhz', 'bandwidthMhz', 'minDbm', 'maxDbm', 'average', 'fps', 'rxAntenna', 'rfGain')
UI = {'audioGain': ('AF gain', 'setValue'), 'audioPan': ('Audio pan', 'setValue'),
      'audioMute': ('Slice audio mute', 'click'), 'agcMode': ('AGC mode', 'setCurrentText'),
      'agcThreshold': ('AGC threshold', 'setValue')}


def plan():
    return {'phase': 'persist-multislice', 'scope': 'one client, two owned slices, one pan; RX-only',
            'scenarios': ['distinct SQL/AGC/filter/audio per slice', 'active slice A/B/A',
                          'SQL Manual/off transitions with distinct slice levels', 'independent mode round trips',
                          'AGC Off slider and distinct off levels before/after restart',
                          'per-slice FM entry/exit retention before explicit cleanup',
                          'normal Quit/relaunch with both slices present',
                          'remove/reopen only test-created slice', 'guarded restoration'],
            'notRun': ['multiple panadapters', 'MultiFlex', 'band-stack topology recall',
                       'physical audio gating', 'TX', 'radio power cycle', 'crash recovery']}


def slice_state(item):
    return {key: item.get(key) for key in FIELDS + SENTINELS}


def by_letter(snapshot):
    result = {}
    for item in snapshot.get('slices', []):
        letter = item.get('letter')
        if letter not in tuple('ABCDEFGH') or letter in result:
            raise StopRun('slice letter identity is missing or ambiguous')
        result[letter] = item
    return result


def pan_state(pan):
    # Flex publishes MHz to six decimals. Compare at that wire resolution;
    # a computed 14.126199999999999 and echoed 14.126200 are the same hertz.
    return {k: round(pan[k], 6) if k in ('centerMhz', 'bandwidthMhz') and type(pan.get(k)) in (int, float)
            else pan.get(k) for k in PAN_FIELDS}


def restoration_conflicts(current, expected):
    """Compare every owned field, preserving type and missing-value failures."""
    return compare(expected, [slice_state(current)])


def agc_slider_field(state):
    return 'flexAgcOffLevel' if state['agcMode'] == 'off' else 'agcThreshold'


def fm_cleanup_state(before, samples, letter):
    """Authorize cleanup of only stable AGC/filter consequences of our FM trip.

    This is not a retention result. The original comparison must be journaled
    first. Never accept missing values, peer changes or unrelated side effects.
    """
    allowed = {'agcMode', 'agcThreshold', 'flexAgcOffLevel', 'filterLow', 'filterHigh'}
    if not samples:
        raise StopRun('FM cleanup has no observations')
    last = samples[-1]
    if set(last) != set(before):
        raise StopRun('FM cleanup slice identities changed')
    for identity, baseline in before.items():
        state = last[identity]
        if (state.get('agcMode') not in ('off', 'slow', 'med', 'fast')
                or any(type(state.get(k)) is not int or not 0 <= state[k] <= 100
                       for k in ('agcThreshold', 'flexAgcOffLevel'))):
            raise StopRun('FM cleanup AGC values are invalid')
        for key, value in baseline.items():
            observed = last[identity].get(key)
            if value is None or observed is None or type(observed) is not type(value):
                raise StopRun('FM cleanup state missing or mistyped')
            if (identity != letter or key not in allowed) and observed != value:
                raise StopRun('FM changed peer or unrelated state; inspect before cleanup')
        for sample in samples:
            if (set(sample) != set(before)
                    or compare(last[identity], [sample.get(identity, {})])['outcome'] != 'ESTABLISHED'):
                raise StopRun('FM cleanup state is not stable')
    return {identity: state.copy() for identity, state in last.items()}


class MultiSliceRun(Run):
    def __init__(self, supervisor, serial, journal):
        super().__init__(supervisor, serial, journal)
        self.count = 1
        self.original = None
        self.extra = None
        self.expected = {}
        self.baselines = {}
        self.pan_expected = None
        # This scenario records per-slice observations, not the single-slice
        # 40-control applet matrix allocated by the shared Run helpers.
        journal.data.pop('appletCoverage', None)

    def assert_ready(self, snapshot):
        return ready(snapshot, self.serial, slice_count=self.count)

    def wait_ready(self, band=None, mode=None):
        deadline, stable = time.monotonic() + 30, None
        reason = 'topology not ready'
        while time.monotonic() < deadline:
            snapshot = self.snapshot()
            try:
                active, _, _ = self.assert_ready(snapshot)
                if mode and active['mode'] != mode:
                    raise StopRun('mode publication pending')
                stable = stable or time.monotonic()
                if time.monotonic() - stable >= 1:
                    return snapshot
            except StopRun as exc:
                stable, reason = None, str(exc)
            time.sleep(.2)
        raise StopRun(reason)

    def action(self, request, reason):
        before = self.snapshot()
        self.assert_ready(before)
        self.journal.event('action-pending', request=request, reason=reason, before=before)
        response = self.supervisor.request(request)
        self.journal.event('action-response', request=request, response=response)

    def select(self, letter):
        snapshot = self.wait_ready()
        slices = by_letter(snapshot)
        if letter not in slices:
            raise StopRun('requested slice identity disappeared')
        self.action({'cmd': 'slice', 'action': 'select', 'value': str(slices[letter]['sliceId'])},
                    'select owned slice ' + letter)
        snapshot = self.wait_ready()
        if not by_letter(snapshot)[letter]['active']:
            raise StopRun('active slice did not follow selection')

    def widget(self, field, value):
        name, action = UI[field]
        target = 'RxApplet/' + name
        self.action({'cmd': 'scrollTo', 'target': target}, 'reveal ' + target)
        request = {'cmd': 'invoke', 'target': target, 'action': action}
        if action != 'click':
            request['value'] = value.capitalize() if field == 'agcMode' else value
        else:
            active, _, _ = self.assert_ready(self.snapshot())
            if active.get(field) == value:
                return
        self.action(request, 'set ' + target)

    def sql(self, on, level):
        self.applets.select_sql('manual')
        self.action({'cmd': 'scrollTo', 'target': 'RxApplet/Squelch threshold'}, 'reveal manual SQL')
        self.action({'cmd': 'invoke', 'target': 'RxApplet/Squelch threshold',
                     'action': 'setValue', 'value': level}, 'set manual SQL threshold')
        if not on:
            self.applets.select_sql('off')

    def set_state(self, letter, state):
        self.select(letter)
        self.action({'cmd': 'tune', 'value': str(state['frequency'])}, 'set slice frequency')
        self.action({'cmd': 'slice', 'action': 'mode', 'value': state['mode']}, 'set slice mode')
        self.wait_ready(mode=state['mode'])
        self.action({'cmd': 'slice', 'action': 'filter',
                     'value': f"{state['filterLow']} {state['filterHigh']}"}, 'set slice passband')
        # One visible slider has two meanings. Set the threshold with AGC on,
        # then the off level with AGC off, finally restore the requested mode.
        self.widget('agcMode', 'med')
        self.widget('agcThreshold', state['agcThreshold'])
        self.widget('agcMode', 'off')
        self.widget('agcThreshold', state['flexAgcOffLevel'])
        self.widget('agcMode', state['agcMode'])
        for field in ('audioGain', 'audioPan', 'audioMute'):
            self.widget(field, state[field])
        self.sql(state['squelch'], state['squelchLevel'])
        self.wait_ready()

    def observe(self, name, expected=None):
        expected = self.expected if expected is None else expected
        wanted = {f'{letter}.{key}': value for letter, state in expected.items() for key, value in state.items()}
        samples = []
        for _ in range(4):
            snapshot = self.snapshot()
            active, _, _ = self.assert_ready(snapshot)
            states = by_letter(snapshot)
            observed = {f'{letter}.{key}': value for letter, item in states.items()
                        for key, value in slice_state(item).items()}
            tree = self.supervisor.request({'cmd': 'dumpTree'})
            letter = active['letter']
            for field, (name_ui, action) in UI.items():
                matches = widgets(tree, 'RxApplet/' + name_ui)
                value = widget_value(matches[0], action) if len(matches) == 1 else None
                if field == 'agcMode' and isinstance(value, str):
                    value = value.lower()
                semantic = agc_slider_field(expected[letter]) if field == 'agcThreshold' else field
                observed[f'ui.{letter}.{semantic}'] = value
                wanted[f'ui.{letter}.{semantic}'] = expected[letter][semantic]
            mode = self.applets.sql_mode()
            observed[f'ui.{letter}.sqlMode'] = mode
            wanted[f'ui.{letter}.sqlMode'] = 'manual' if expected[letter]['squelch'] else 'off'
            if expected[letter]['squelch']:
                matches = widgets(tree, 'RxApplet/Squelch threshold')
                observed[f'ui.{letter}.sqlLevel'] = widget_value(matches[0], 'setValue') if len(matches) == 1 else None
                wanted[f'ui.{letter}.sqlLevel'] = expected[letter]['manualSquelchLevel']
            self.journal.event('slice-observation', scenario=name, snapshot=snapshot, observed=observed)
            samples.append(observed)
            time.sleep(.2)
        return self.journal.result(name, wanted, samples)

    def off_roundtrip(self, letter, phase):
        self.select(letter)
        mode = self.expected[letter]['agcMode']
        self.widget('agcMode', 'off')
        self.wait_ready()
        self.expected[letter]['agcMode'] = 'off'
        result = self.observe(phase + ' AGC Off slider ' + letter)
        if result['outcome'] != 'ESTABLISHED':
            raise StopRun('AGC Off model/slider disagreement; inspect before continuing')
        self.widget('agcMode', mode)
        self.expected[letter]['agcMode'] = mode
        self.wait_ready()
        if self.observe(phase + ' AGC enabled slider ' + letter)['outcome'] != 'ESTABLISHED':
            raise StopRun('AGC enabled model/slider disagreement')

    def fm_roundtrip(self, letter):
        self.select(letter)
        before = {identity: state.copy() for identity, state in self.expected.items()}
        if self.observe('before FM ' + letter)['outcome'] != 'ESTABLISHED':
            raise StopRun('FM baseline disagrees; refusing transition')
        self.action({'cmd': 'slice', 'action': 'mode', 'value': 'FM'}, 'enter FM without AGC replay')
        self.wait_ready(mode='FM')
        for _ in range(4):
            snapshot = self.snapshot()
            self.assert_ready(snapshot)
            self.journal.event('FM-entry-observation', letter=letter, snapshot=snapshot,
                               tree=self.supervisor.request({'cmd': 'dumpTree'}))
            time.sleep(.2)
        self.action({'cmd': 'slice', 'action': 'mode', 'value': before[letter]['mode']},
                    'leave FM without AGC replay')
        self.wait_ready(mode=before[letter]['mode'])
        # Keep the retention concern even when later cleanup succeeds.
        self.observe('FM return retention ' + letter, before)
        samples = []
        for _ in range(4):
            snapshot = self.snapshot()
            self.assert_ready(snapshot)
            samples.append({identity: slice_state(item) for identity, item in by_letter(snapshot).items()})
            self.journal.event('FM-cleanup-observation', letter=letter, snapshot=snapshot)
            time.sleep(.2)
        cleanup_expected = fm_cleanup_state(before, samples, letter)
        self.journal.event('FM-cleanup-intent', retentionExpected=before, observed=cleanup_expected,
                           reason='explicit test cleanup; never a production AGC replay')
        self.expected = cleanup_expected
        # Close the observation/action gap as far as the bridge permits.
        current = {identity: slice_state(item) for identity, item in by_letter(self.wait_ready()).items()}
        if current != cleanup_expected:
            raise StopRun('state changed before FM cleanup; refusing overwrite')
        self.set_state(letter, before[letter])
        self.expected = before
        result = self.observe('explicit FM cleanup ' + letter)
        self.journal.data['cleanup'].append({'fmSlice': letter, **result})
        self.journal.save()
        if result['outcome'] != 'ESTABLISHED':
            raise StopRun('FM cleanup did not converge')

    def add_extra(self):
        before = self.wait_ready()
        if self.count != 1 or before['radio'].get('maxSlices', 0) < 2:
            raise StopRun('additional slice capability unavailable')
        self.action({'cmd': 'slice', 'action': 'add', 'value': '14.160'}, 'create test-owned additional slice')
        self.count = 2
        after = self.wait_ready()
        added = set(by_letter(after)) - set(by_letter(before))
        if len(added) != 1:
            raise StopRun('new slice identity is ambiguous')
        self.extra = added.pop()
        self.baselines[self.extra] = slice_state(by_letter(after)[self.extra])
        self.expected[self.extra] = self.baselines[self.extra].copy()
        self.journal.event('created-slice-baseline', letter=self.extra, snapshot=after)

    def remove_extra(self):
        if not self.extra or self.extra == self.original:
            raise StopRun('refusing to remove a slice not created by this run')
        snapshot = self.wait_ready()
        item = by_letter(snapshot)[self.extra]
        conflict = restoration_conflicts(item, self.expected[self.extra])
        if conflict['outcome'] != 'ESTABLISHED':
            raise StopRun('test slice changed unexpectedly; refusing removal')
        self.select(self.original)
        self.action({'cmd': 'slice', 'action': 'remove', 'value': str(item['sliceId'])}, 'remove only test-created slice')
        del self.expected[self.extra]
        self.count = 1
        self.wait_ready()
        self.extra = None

    def restore(self, letter):
        snapshot = self.wait_ready()
        conflict = restoration_conflicts(by_letter(snapshot)[letter], self.expected[letter])
        if conflict['outcome'] != 'ESTABLISHED':
            self.journal.data['cleanup'].append({'slice': letter, **conflict})
            self.journal.save()
            raise StopRun('newer or unexpected slice state; refusing restoration')
        baseline = self.baselines[letter]
        self.set_state(letter, baseline)
        self.expected[letter] = baseline.copy()
        result = self.observe('restore slice ' + letter)
        self.journal.data['cleanup'].append({'slice': letter, **result})
        self.journal.save()
        if result['outcome'] != 'ESTABLISHED':
            raise StopRun('restoration did not converge')

    def restore_pan(self, original):
        current = self.wait_ready()['pans'][0]
        if compare(pan_state(self.pan_expected or {}), [pan_state(current)])['outcome'] != 'ESTABLISHED':
            raise StopRun('pan changed since our last intentional tuning; refusing restoration')
        # Slice tuning can invoke the existing reveal/autopan policy even when
        # both frequencies fit inside the full span. Only center was exercised.
        if any(current.get(k) != original.get(k) for k in PAN_FIELDS if k != 'centerMhz'):
            raise StopRun('unexercised pan setting changed; refusing restoration')
        if current['centerMhz'] != original['centerMhz']:
            self.action({'cmd': 'pan', 'action': 'center', 'value': str(original['centerMhz'])},
                        'restore center moved by production slice reveal')
        final = self.wait_ready()
        result = self.journal.result('restore original pan center',
                                    pan_state(original), [pan_state(final['pans'][0])])
        self.journal.data['cleanup'].append({'pan': result})
        self.journal.save()
        if result['outcome'] != 'ESTABLISHED':
            raise StopRun('pan restoration did not converge')
        return final

    def execute(self):
        self.connect()
        initial = self.wait_ready()
        self.original = self.assert_ready(initial)[0]['letter']
        self.baselines[self.original] = slice_state(by_letter(initial)[self.original])
        self.expected[self.original] = self.baselines[self.original].copy()
        if self.baselines[self.original]['mode'] not in ('USB', 'LSB'):
            raise StopRun('v1 two-slice plan requires an initial USB/LSB context for reversible SQL controls')
        pan = initial['pans'][0]
        half = pan['bandwidthMhz'] / 2
        if not all(pan['centerMhz'] - half < f < pan['centerMhz'] + half for f in (14.180, 14.160)):
            raise StopRun('both RX seed frequencies must fit inside the original pan; no implicit recenter')
        self.journal.event('original-topology', snapshot=initial)
        self.add_extra()
        for index, letter in enumerate((self.original, self.extra)):
            state = {**self.expected[letter], 'frequency': (14.180, 14.160)[index],
                     'mode': 'USB' if index == 0 else 'LSB',
                     'filterLow': 150 if index == 0 else -2650,
                     'filterHigh': 2450 if index == 0 else -250,
                     'agcMode': 'fast' if index == 0 else 'slow', 'agcThreshold': 43 + index * 18,
                     'flexAgcOffLevel': 17 + index * 12,
                     'audioGain': 23 + index * 18, 'audioPan': 25 + index * 50,
                     'audioMute': False, 'squelch': True, 'squelchLevel': 26 + index * 13,
                     'manualSquelchLevel': 26 + index * 13}
            self.journal.event('slice-seed-intent', letter=letter, state=state)
            self.set_state(letter, state)
            seeded_pan = self.wait_ready()['pans'][0]
            self.pan_expected = pan_state(seeded_pan)
            self.journal.event('owned-pan-after-seed', expected=self.pan_expected)
            self.expected[letter] = state
            if self.observe('seed ' + letter)['outcome'] != 'ESTABLISHED':
                raise StopRun('seed disagreement; retention cannot be inferred')
        for letter in (self.original, self.extra):
            self.off_roundtrip(letter, 'seeded')
        for letter in (self.original, self.extra, self.original):
            self.select(letter)
            self.observe('active selection ' + letter)
        self.select(self.extra)
        self.applets.select_sql('off')
        self.expected[self.extra]['squelch'] = False
        self.wait_ready()
        self.observe('second slice SQL off, first unchanged')
        self.select(self.original)
        self.observe('first slice SQL remains manual')
        self.select(self.extra)
        self.applets.select_sql('manual')
        self.expected[self.extra]['squelch'] = True
        self.wait_ready()
        self.observe('second slice SQL manual restored')
        for letter in (self.original, self.extra):
            self.select(letter)
            mode = self.expected[letter]['mode']
            self.action({'cmd': 'slice', 'action': 'mode', 'value': 'LSB' if mode == 'USB' else 'USB'}, 'independent slice mode outward')
            self.wait_ready()
            self.action({'cmd': 'slice', 'action': 'mode', 'value': mode}, 'independent slice mode return')
            self.wait_ready(mode=mode)
            self.observe('mode round trip ' + letter)
        self.select(self.original)
        self.supervisor.quit()
        self.supervisor.launch()
        self.connect()
        self.observe('two slices after normal restart')
        for letter in (self.extra, self.original):
            self.select(letter)
            self.observe('post-restart selected ' + letter)
        for letter in (self.original, self.extra):
            self.off_roundtrip(letter, 'post-restart')
            self.fm_roundtrip(letter)
        self.restore(self.extra)
        self.remove_extra()
        self.observe('original survives removal')
        self.add_extra()
        self.observe('original survives reopen; new slice inventoried')
        self.remove_extra()
        self.restore(self.original)
        final = self.restore_pan(initial['pans'][0])
        self.journal.event('final-restoration', snapshot=final,
                           panBefore=initial['pans'], panAfter=final['pans'])
        self.supervisor.request({'cmd': 'log', 'action': 'reset'})
        self.supervisor.quit()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('command', choices=('plan', 'run'))
    parser.add_argument('--app', type=Path)
    parser.add_argument('--serial')
    parser.add_argument('--profile', type=Path)
    parser.add_argument('--output', type=Path)
    args = parser.parse_args()
    if args.command == 'plan':
        import json
        print(json.dumps(plan(), indent=2))
        return 0
    if sys.platform == 'win32':
        parser.error('process supervision currently supports macOS/Linux')
    if not all((args.app, args.serial, args.profile, args.output)):
        parser.error('run requires --app --serial --profile --output')
    app = args.app.resolve()
    if app.suffix == '.app':
        app /= 'Contents/MacOS/AetherSDR'
    if not app.is_file() or not os.access(app, os.X_OK):
        parser.error('app executable is missing or not executable')
    profile, output = args.profile.resolve(), args.output.resolve()
    if (profile.exists() or output.exists() or profile == output
            or profile in output.parents or output in profile.parents):
        parser.error('profile/output must be new, separate, non-nested directories')
    profile.mkdir(parents=True, mode=0o700)
    output.mkdir(parents=True, mode=0o700)
    journal = Journal(output / 'persist.json', {'plan': plan(), 'app': str(app),
                      'sha256': hashlib.sha256(app.read_bytes()).hexdigest()})
    sources = {}
    for name in ('radiocert_persist_multislice.py', 'radiocert_persist.py', 'radiocert_persist_applets.py'):
        source = Path(__file__).with_name(name).read_bytes()
        sources[name] = hashlib.sha256(source).hexdigest()
        (output / name).write_bytes(source)
    journal.data['metadata']['runnerSources'] = sources
    journal.save()
    supervisor = Supervisor(app, profile, output, journal)
    try:
        supervisor.initialize_profile()
        supervisor.launch()
        MultiSliceRun(supervisor, args.serial, journal).execute()
        journal.data['status'] = 'completed'
    except (StopRun, OSError, ConnectionError, KeyError, ValueError, subprocess.TimeoutExpired, KeyboardInterrupt) as exc:
        journal.data['status'] = 'interrupted'
        journal.event('stopped', reason=str(exc), recovery='Inspect journal; no blind restore or removal of original slices.')
        print(str(exc), flush=True)
        return 2
    finally:
        journal.save()
        journal.report()
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
