"""Capability/visibility-gated, RX-only applet contracts for RadioCert Persist.

The catalogue is data, never arbitrary mutation text supplied by a radio. Fields
without an actual reachable UI route remain explicit gaps, not coverage.
"""
from dataclasses import dataclass
import time


@dataclass(frozen=True)
class Control:
    field: str
    target: str
    seed: object
    alternate: object
    action: str = 'setValue'
    owner: str = 'radio'
    scope: str = 'operating context; transition behavior under diagnosis'


CONTROLS = (
    Control('transmit.rfPower', 'TxApplet/RF power', 12, 9),
    Control('transmit.tunePower', 'TxApplet/Tune power', 8, 6),
    Control('slice.audioGain', 'RxApplet/AF gain', 23, 27, scope='slice'),
    Control('slice.audioPan', 'RxApplet/Audio pan', 31, 37, scope='slice'),
    Control('slice.audioMute', 'RxApplet/Slice audio mute', True, False, 'click', scope='slice'),
    Control('slice.manualSquelchLevel', 'RxApplet/Squelch threshold', 26, 32, scope='slice manual intent'),
    Control('slice.agcMode', 'RxApplet/AGC mode', 'fast', 'slow', 'setCurrentText', scope='slice/mode'),
    Control('slice.agcThreshold', 'RxApplet/AGC threshold', 47, 53, scope='slice/mode'),
    Control('transmit.micLevel', 'PhoneCwApplet/Microphone gain', 37, 41),
    Control('transmit.speechProc', 'PhoneCwApplet/Speech processor', True, False, 'setChecked'),
    Control('transmit.speechProcLevel', 'PhoneCwApplet/Processor level', 1, 2),
    Control('transmit.monitor', 'PhoneCwApplet/TX monitor', True, False, 'setChecked'),
    Control('transmit.monGainSb', 'PhoneCwApplet/Monitor volume', 29, 33),
    Control('transmit.amCarrierLevel', 'PhoneApplet/AM carrier level', 41, 43),
    Control('transmit.voxLevel', 'PhoneApplet/VOX level', 34, 38),
    Control('transmit.voxDelay', 'PhoneApplet/VOX delay', 29, 31),
    Control('transmit.dexp', 'PhoneApplet/Downward expander', True, False, 'setChecked'),
    Control('transmit.dexpLevel', 'PhoneApplet/DEXP threshold', 33, 37),
)
EQ_BANDS = ('63', '125', '250', '500', '1k', '2k', '4k', '8k')
EQ_CONTROLS = tuple(
    Control(f'equalizer.{side}.{band}', f'EqApplet/EQ {band}',
            (-1 if side == 'rx' else 1) * (i % 5 + 1),
            (1 if side == 'rx' else -1) * (i % 5 + 1), scope=f'{side.upper()} radio EQ')
    for side in ('rx', 'tx') for i, band in enumerate(EQ_BANDS)) + tuple(
    Control(f'equalizer.{side}Enabled', 'EqApplet/Equalizer enable', True, False,
            'setChecked', scope=f'{side.upper()} radio EQ') for side in ('rx', 'tx'))
CW_CONTROLS = (
    Control('transmit.cwDelay', 'PhoneCwApplet/CW delay', 170, 190),
    Control('transmit.cwSpeed', 'PhoneCwApplet/CW speed', 23, 27),
    Control('transmit.monGainCw', 'PhoneCwApplet/Sidetone volume', 19, 21),
    Control('transmit.monPanCw', 'PhoneCwApplet/CW audio pan', 37, 43),
)
ALL_CONTROLS = CONTROLS + EQ_CONTROLS + CW_CONTROLS
GAPS = {
    'voxEnable/cwBreakIn/PTT/TUNE/ATU': 'excluded: no transmit authorization',
    'global/TX/mic profile loads': 'excluded: compound transition may arm transmission',
    'mic boost/bias and accessory delays': 'observable; Radio Setup UI scenario pending',
    'AGC OFF and automatic SQL intent': 'separate semantic contracts pending',
    'radio RX DSP and FM/repeater': 'observable; mode-specific UI scenarios pending',
    'client DSP/CHAIN': 'partial observation; per-stage persistence scenarios pending',
    'audio devices/master volume/routing': 'partial observation; device lifecycle scenarios pending',
    'applet layout/geometry': 'tree inventory only; geometry scenarios pending',
    'RIT/XIT/step/DAX channels': 'observable; per-slice scenario pending',
}


def flatten(value, prefix=''):
    result = {}
    if isinstance(value, dict):
        for name, item in value.items():
            result.update(flatten(item, prefix + '.' + name if prefix else name))
    elif not isinstance(value, list):
        result[prefix] = value
    return result


def widgets(tree, target):
    """Same ancestor/identity shape as the bridge; ambiguity stays explicit."""
    scope, name = target.rsplit('/', 1) if '/' in target else ('', target)
    found = []
    def visit(node, ancestors=()):
        if isinstance(node, dict):
            identities = (node.get('class', '').split('::')[-1], node.get('objectName'),
                          node.get('accessibleName'), node.get('text'))
            if name in identities and (not scope or scope in ancestors):
                found.append(node)
            for child in node.get('children', []):
                visit(child, ancestors + identities)
            for child in node.get('roots', []):
                visit(child, ancestors)
        elif isinstance(node, list):
            for child in node:
                visit(child, ancestors)
    visit(tree)
    return found


def widget_value(widget, action):
    if action == 'setChecked':
        return widget.get('checked')
    if action == 'click' and widget.get('accessibleName') == 'Slice audio mute':
        # The actual mute button is an icon button, not a checkable toggle.
        return {'🔇': True, '🔊': False}.get(widget.get('value'))
    value = widget.get('value')
    if 'range' in widget and isinstance(value, str):
        try:
            return int(value)
        except ValueError:
            return value
    return value


def initial_settling(expected, samples, field, baseline):
    """Only a leading pre-action value may settle; regressions never disappear.

    Return the leading observations separately. Callers keep the unmodified
    snapshots in the journal. An unrelated field changing, an intermediate third
    value, or a reversion after convergence remains a concern.
    """
    if field not in expected or baseline == expected[field]:
        return 0
    first_match = next((i for i, sample in enumerate(samples)
                        if sample.get(field) == expected[field]
                        and type(sample.get(field)) is type(expected[field])), None)
    if not first_match:
        return 0
    for sample in samples[:first_match]:
        for key, value in expected.items():
            want = baseline if key == field else value
            if key not in sample or type(sample[key]) is not type(want) or sample[key] != want:
                return 0
    return first_match


class AppletRun:
    def __init__(self, run):
        self.run = run
        self.expected = {}
        self.saved = {}
        self.attempted = []
        self.sql_baseline = None
        self.coverage = {c.field: {'target': c.target, 'owner': c.owner, 'scope': c.scope,
                                 'inventoried': True, 'observable': False,
                                 'actionExercised': False, 'transitions': {}}
                         for c in ALL_CONTROLS}
        run.journal.data['appletCoverage'] = self.coverage
        run.journal.data['coverageGaps'] = GAPS

    def snapshot(self):
        snapshot = self.run.snapshot()
        self.run.assert_ready(snapshot)
        return snapshot

    def sql_mode(self):
        tree = self.run.supervisor.request({'cmd': 'dumpTree'})
        button = widgets(tree, 'RxApplet/Squelch mode')
        slider = widgets(tree, 'RxApplet/Squelch threshold')
        if len(button) != 1 or len(slider) != 1:
            raise ValueError('ambiguous SQL surface')
        if button[0].get('text', button[0].get('value')) == 'AUTO':
            return 'auto'
        return 'manual' if slider[0].get('enabled') else 'off'

    def select_sql(self, mode):
        for _ in range(3):
            if self.sql_mode() == mode:
                return
            self.run.action({'cmd': 'invoke', 'target': 'RxApplet/Squelch mode',
                             'action': 'click'}, 'cycle SQL intent to ' + mode)
        if self.sql_mode() != mode:
            raise ValueError('SQL intent did not converge')

    def show(self, control):
        # Scroll the real applet; never force-enable a disabled control. Applets
        # are normally open in a new test profile; a closed one uses its tray.
        if control.field == 'slice.manualSquelchLevel':
            if self.sql_baseline is None:
                self.sql_baseline = self.sql_mode()
                self.run.journal.event('sql-intent-baseline', mode=self.sql_baseline)
            self.select_sql('manual')
        if control.field.startswith('equalizer.'):

            side = 'rx' if control.field.split('.')[1].startswith('rx') else 'tx'
            self.run.action({'cmd': 'invoke', 'target': f'EqApplet/{side.upper()} equalizer',
                             'action': 'click'}, 'select radio EQ curve')
        self.run.action({'cmd': 'scrollTo', 'target': control.target}, 'reveal applet control')
        matches = widgets(self.run.supervisor.request({'cmd': 'dumpTree'}), control.target)
        if len(matches) != 1:
            return None, f'UI target count {len(matches)}'
        widget = matches[0]
        if not widget.get('visible') or not widget.get('enabled') or widget.get('keying'):
            return None, 'UI hidden, disabled or keying; no bypass'
        return widget, None

    def set_control(self, control, value, reason):
        # Explicit allow-list only. Enabling VOX/keying is never a generated action.
        if control not in ALL_CONTROLS:
            raise ValueError('control is outside RX-only applet allow-list')
        if control.action == 'click':
            snapshot = self.snapshot()
            current = flatten({**snapshot, 'slice': snapshot['slices'][0]})[control.field]
            if current != value:
                self.run.action({'cmd': 'invoke', 'target': control.target, 'action': 'click'}, reason)
            return
        ui_value = value.title() if control.field == 'slice.agcMode' else value
        self.run.action({'cmd': 'invoke', 'target': control.target, 'action': control.action,
                         'value': ui_value}, reason)

    def observe(self, name, expected=None, samples=5, settling_field=None):
        from radiocert_persist import compare

        wanted = dict(self.expected if expected is None else expected)
        controls = [control for control in ALL_CONTROLS if control.field in wanted]
        ui_expected = {'ui.' + c.field: wanted[c.field].title()
                       if c.field == 'slice.agcMode' else wanted[c.field] for c in controls}
        observed = []
        for _ in range(samples):
            snapshot = self.snapshot()
            sample = flatten({**snapshot, 'slice': snapshot['slices'][0]})
            tree = self.run.supervisor.request({'cmd': 'dumpTree'})
            rx = widgets(tree, 'EqApplet/RX equalizer')
            selected_eq = ('rx' if rx[0].get('checked') is True else 'tx'
                           if rx[0].get('checked') is False else None) if len(rx) == 1 else None
            ui = {}
            for control in controls:
                matches = widgets(tree, control.target)
                side = ('rx' if control.field.split('.')[1].startswith('rx') else 'tx') \
                    if control.field.startswith('equalizer.') else None
                available = (len(matches) == 1 and matches[0].get('visible')
                             and matches[0].get('enabled') and (side is None or side == selected_eq))
                # Do not switch pages or repair controls during observation.
                # Unobserved presentation stays an explicit coverage gap.
                value = widget_value(matches[0], control.action) if available else None
                key = 'ui.' + control.field
                sample[key] = value
                ui[control.field] = {'expected': ui_expected[key], 'observed': value,
                                     'outcome': compare({key: ui_expected[key]}, [{key: value}])['outcome']}
            self.run.journal.event('applet-widget-observation', scenario=name, observations=ui,
                                   limitation='Hidden, disabled, missing or unselected controls are not verified.')
            observed.append(sample)
            time.sleep(.5)
        # Settling applies to the model's leading baseline only. Always retain
        # every widget sample, including a mismatch that later recovers.
        settled_after = initial_settling(wanted, observed, settling_field,
                                        self.saved.get(settling_field)) if settling_field else 0
        if settled_after:
            self.run.journal.event('initial-settling', scenario=name, field=settling_field,
                                   leadingSamples=observed[:settled_after], settledAfterSample=settled_after)
        combined = {**wanted, **ui_expected}
        evidence = []
        for index, sample in enumerate(observed):
            evidence.append({**sample, **(wanted if index < settled_after else {})})
        prerequisites = {field: row['seedOutcome'] for field, row in self.coverage.items()
                         if field in wanted and row.get('seedOutcome') not in (None, 'ESTABLISHED')}
        retention = not name.startswith('set ') and name != 'cleanup'
        result = self.run.journal.result('applets: ' + name, combined, evidence,
                                         prerequisites if retention else None)
        if settled_after:
            self.run.journal.data['results'][-1]['initialSettlingSamples'] = settled_after
        for field in wanted:
            if field not in self.coverage:
                continue
            keys = (field, 'ui.' + field)
            field_expected = {key: combined[key] for key in keys if key in combined}
            outcome = compare(field_expected, evidence)['outcome']
            if retention and field in prerequisites and outcome == 'ESTABLISHED':
                outcome = 'INCONCLUSIVE'
            self.coverage[field]['transitions'][name] = outcome
            ui_key = 'ui.' + field
            if ui_key in ui_expected:
                self.coverage[field].setdefault('widgetTransitions', {})[name] = compare(
                    {ui_key: ui_expected[ui_key]}, evidence)
        if 'slice.manualSquelchLevel' in wanted:
            mode = self.sql_mode()
            self.coverage['slice.manualSquelchLevel'].setdefault('intentObservations', {})[name] = mode
            self.run.journal.event('sql-intent-observation', scenario=name, mode=mode)
        self.run.journal.save()
        return result

    def seed(self, controls=CONTROLS + EQ_CONTROLS):
        for control in controls:
            row = self.coverage[control.field]
            snap = self.snapshot()
            state = flatten({**snap, 'slice': snap['slices'][0]})
            row['observable'] = control.field in state
            if not row['observable']:
                row['reason'] = 'field unavailable on this capability'; continue
            # Capture the restore contract before even selecting the EQ page.
            old = state[control.field]
            self.saved[control.field] = old
            widget, reason = self.show(control)
            if reason:
                row['reason'] = reason; continue
            value = control.alternate if old == control.seed else control.seed
            self.attempted.append(control)
            self.expected[control.field] = value
            self.run.journal.event('applet-baseline', field=control.field, value=old,
                                   widget=widget, seed=value)
            self.set_control(control, value, 'seed ' + control.field)
            row['actionExercised'] = True
            time.sleep(.25)
            # Assert earlier seeds too: unrelated status fan-out can reset them.
            self.observe('set ' + control.field, samples=5, settling_field=control.field)
            row['seedOutcome'] = row['transitions']['set ' + control.field]
            row['widgetReadback'] = row['widgetTransitions']['set ' + control.field]
            self.run.journal.save()

    def bypass_roundtrips(self):
        fields = ('transmit.speechProc', 'transmit.dexp', 'transmit.monitor',
                  'equalizer.rxEnabled', 'equalizer.txEnabled')
        for field in fields:
            if field not in self.expected:
                continue
            control = next(c for c in ALL_CONTROLS if c.field == field)
            value = self.expected[field]
            widget, reason = self.show(control)
            if reason:
                self.coverage[field]['bypassGap'] = reason; continue
            self.set_control(control, not value, 'bypass round trip ' + field)
            time.sleep(.75)
            self.observe('alternate enable ' + field, {**self.expected, field: not value})
            self.set_control(control, value, 'return enable ' + field)
            time.sleep(.75)
            self.observe('enable round trip ' + field)

    def restore(self, controls=None):
        selected = list(reversed(self.attempted)) if controls is None else list(reversed(controls))
        restored = {}
        for control in selected:
            if control not in self.attempted:
                continue
            snap = self.snapshot()
            state = flatten({**snap, 'slice': snap['slices'][0]})
            row = self.coverage[control.field]
            if state.get(control.field) != self.expected[control.field]:
                row['cleanup'] = 'INCONCLUSIVE: changed since test intent; retained for review'
                continue
            widget, reason = self.show(control)
            if reason:
                row['cleanup'] = 'INCONCLUSIVE: ' + reason; continue
            self.set_control(control, self.saved[control.field], 'restore ' + control.field)
            restored[control.field] = self.saved[control.field]
            self.expected.pop(control.field)
            self.attempted.remove(control)
        if controls is None and self.sql_baseline is not None:
            self.select_sql(self.sql_baseline)
        if restored:
            result = self.observe('cleanup', restored)
            for field in restored:
                self.coverage[field]['cleanup'] = self.coverage[field]['transitions']['cleanup']
        self.run.journal.save()
