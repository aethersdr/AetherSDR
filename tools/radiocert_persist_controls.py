#!/usr/bin/env python3
"""RX-only control-contract Persist scenario for radio-owned controls (Flex, HL2, Icom).

Three contracts, each the shape of an IC-7300MK2 defect a model test agreed with:

  range     a mid-range value of a published 0..max control holds through two
            periodic polls and comes back after a full restart, where the
            connect-time read is its only writer. The processor level was
            written in one domain and decoded in another, so NOR and DX
            snapped to DX+ (#6174).
  boundary  manual SQL at threshold 0 keeps Manual through two polls. With no
            squelch enable on an Icom, "on at 0" read back as Off and stuck
            (#6175).
  restart   a distinctive manual SQL level comes back from the radio.

The preamp/ATT interlock and the SQL line's scale are in-session measurements
in `radiocert meters` (front-end-interlock, squelch-scale), not here.

An Icom connects by host. The runner never handles credentials: complete the
sign-in in the client window at each launch.
"""
import argparse
import hashlib
import json
import os
import subprocess
import sys
import time
from pathlib import Path

from radiocert_persist import Journal, StopRun, Supervisor, compare
from radiocert_persist_applets import flatten, widgets, widget_value

FAMILIES = ('flex', 'hl2', 'icom')
# Two IC-7300MK2 controls polls (3 s each) plus margin, at 0.5 s per sample.
HOLD_SAMPLES = 15
PROC_LEVEL = 'PhoneCwApplet/Processor level'
PROC_ENABLE = 'PhoneCwApplet/Speech processor'
SQL_MODE = 'RxApplet/Squelch mode'
SQL_LEVEL = 'RxApplet/Squelch threshold'
SQL_SEEDS = (26, 32)


def plan():
    return {'phase': 'persist-controls', 'family': list(FAMILIES),
            'scope': 'one isolated client, one slice; receive-only',
            'scenarios': ['processor level at a mid-range value of the published maximum',
                          'manual SQL at threshold 0 holds Manual through two polls',
                          'distinctive manual SQL level', 'normal Quit and same-profile relaunch',
                          'guarded restoration of seeded fields'],
            'notRun': ['front-end interlock and SQL line scale (radiocert meters)',
                       'SQL at 0 across restart (a radio with no squelch enable reads it as Off)',
                       'TX', 'band/mode contexts', 'radio power cycle', 'crash recovery']}


def range_seeds(maximum):
    """Two distinct interior values of 0..maximum, away from both ends.

    Mid-range is where a percent decode of a stepped control lands elsewhere;
    the ends are probed in-session by radiocert meters. For 0..10 these are 3
    and 6, the MK2 bins measured live (0081 -> 3, 0151 -> 6)."""
    if type(maximum) is not int or maximum < 2:
        raise ValueError('published maximum must be an integer of at least 2')
    first, second = round(0.3 * maximum), round(0.6 * maximum)
    first = max(first, 1)
    if second <= first:
        second = min(first + 1, maximum)
    return first, second


def ready_controls(snapshot, identity, *, tx_permission=False):
    """Safety and identity before every mutation. `identity` pins family/model/serial."""
    if snapshot.get('schemaVersion') != 1:
        raise StopRun('unsupported persist snapshot schema')
    who, radio = snapshot.get('identity', {}), snapshot.get('radio', {})
    if who.get('txAllowed') is not tx_permission or who.get('readOnly') is not False:
        raise StopRun('bridge permission does not match the explicit scenario')
    if snapshot.get('family') not in FAMILIES:
        raise StopRun(f"family {snapshot.get('family')!r} has no control contracts")
    if radio.get('connected') is not True:
        raise StopRun('radio is disconnected')
    for key, value in identity.items():
        if (snapshot.get('family') if key == 'family' else radio.get(key)) != value:
            raise StopRun(f'radio identity changed: {key}')
    if radio.get('transmitting') is not False:
        raise StopRun('radio is transmitting or TX state is unknown')
    tx = snapshot.get('transmit', {})
    if any(tx.get(field) is not False for field in ('transmitting', 'tuning', 'mox', 'voxEnable')):
        raise StopRun('TX/VOX is active or unknown; receive-only run cannot proceed')
    slices = snapshot.get('slices', [])
    if len(slices) != 1:
        raise StopRun('scenario requires exactly one slice')
    return slices[0]


def state(snapshot):
    return flatten({'transmit': snapshot.get('transmit', {}), 'slice': snapshot['slices'][0]})


def sql_mode(tree):
    """Off, manual or auto, read from the RX applet the operator sees."""
    button, slider = widgets(tree, SQL_MODE), widgets(tree, SQL_LEVEL)
    if len(button) != 1 or len(slider) != 1:
        return None
    if button[0].get('text', button[0].get('value')) == 'AUTO':
        return 'auto'
    return 'manual' if slider[0].get('enabled') else 'off'


class ControlsRun:
    def __init__(self, supervisor, journal, *, serial=None, icom_host=None, sign_in_seconds=300):
        if bool(serial) == bool(icom_host):
            raise ValueError('exactly one of serial or icom_host')
        self.supervisor, self.journal = supervisor, journal
        self.serial, self.icom_host, self.sign_in_seconds = serial, icom_host, sign_in_seconds
        self.identity = {}
        self.baseline, self.expected = {}, {}
        self.sql_baseline = None

    # ---- bridge ----
    def snapshot(self):
        snapshot = self.supervisor.request({'cmd': 'radiocert', 'action': 'persist'})
        self.journal.event('snapshot', snapshot=snapshot)
        return snapshot

    def tree(self):
        return self.supervisor.request({'cmd': 'dumpTree'})

    def action(self, request, reason):
        before = self.snapshot()
        ready_controls(before, self.identity)
        self.journal.event('action-pending', request=request, reason=reason, before=before)
        response = self.supervisor.request(request)
        self.journal.event('action-response', request=request, response=response)

    def connect(self):
        snapshot = self.snapshot()
        if not snapshot.get('radio', {}).get('connected'):
            if self.icom_host:
                self.journal.event('connect-pending', host=self.icom_host, family='icom')
                self.supervisor.request({'cmd': 'connect', 'action': 'ip',
                                         'value': f'{self.icom_host} icom'})
                print('Complete the Icom sign-in in the RadioCert Persist client window; '
                      'this runner never handles credentials.', flush=True)
                deadline = time.monotonic() + self.sign_in_seconds
            else:
                radios = self.supervisor.request({'cmd': 'connect', 'action': 'list'}).get('radios', [])
                matches = [r for r in radios if r.get('serial') == self.serial]
                if len(matches) != 1 or str(matches[0].get('status', '')).lower() != 'available':
                    raise StopRun('exact serial is not discovered Available; no fallback')
                self.journal.event('connect-pending', radio=matches[0])
                self.supervisor.request({'cmd': 'connect', 'action': 'local',
                                         'value': 'serial ' + self.serial})
                deadline = time.monotonic() + 30
            while not self.snapshot().get('radio', {}).get('connected'):
                if time.monotonic() > deadline:
                    raise StopRun('radio did not connect in time')
                time.sleep(1)
        return self.wait_ready()

    def wait_ready(self):
        deadline, stable_since, last = time.monotonic() + 30, None, 'radio not ready'
        while time.monotonic() < deadline:
            snapshot = self.snapshot()
            try:
                if not self.identity:
                    radio = snapshot.get('radio', {})
                    self.identity = {'family': snapshot.get('family'), 'model': radio.get('model')}
                    if self.serial:
                        self.identity['serial'] = self.serial
                    self.journal.event('identity-pinned', identity=self.identity)
                ready_controls(snapshot, self.identity)
                stable_since = stable_since or time.monotonic()
                if time.monotonic() - stable_since >= 2:
                    return snapshot
            except StopRun as exc:
                last, stable_since = str(exc), None
            time.sleep(0.5)
        raise StopRun(last)

    # ---- controls through the operator's widgets ----
    def reveal(self, target):
        self.action({'cmd': 'scrollTo', 'target': target}, 'reveal applet control')
        matches = widgets(self.tree(), target)
        if len(matches) != 1:
            return f'UI target count {len(matches)}'
        if not matches[0].get('visible') or not matches[0].get('enabled'):
            return 'UI hidden or disabled; no bypass'
        return None

    def set_widget(self, target, action, value, reason):
        gap = self.reveal(target)
        if gap:
            raise StopRun(f'{target}: {gap}')
        self.action({'cmd': 'invoke', 'target': target, 'action': action, 'value': value}, reason)

    def select_sql(self, mode):
        for _ in range(3):
            if sql_mode(self.tree()) == mode:
                return
            self.action({'cmd': 'invoke', 'target': SQL_MODE, 'action': 'click'},
                        'cycle SQL intent to ' + mode)
        if sql_mode(self.tree()) != mode:
            raise StopRun('SQL intent did not converge')

    # ---- evidence ----
    def observe(self, name, expected, widgets_expected=None, samples=HOLD_SAMPLES, intent=None):
        """Every sample must match; a later recovery never hides an earlier miss."""
        observed = []
        for _ in range(samples):
            sample = state(self.snapshot())
            tree = self.tree()
            for target, (action, _) in (widgets_expected or {}).items():
                matches = widgets(tree, target)
                sample['ui.' + target] = widget_value(matches[0], action) if len(matches) == 1 else None
            if intent is not None:
                sample['ui.sqlMode'] = sql_mode(tree)
            observed.append(sample)
            time.sleep(0.5)
        wanted = dict(expected)
        wanted.update({'ui.' + t: v for t, (_, v) in (widgets_expected or {}).items()})
        if intent is not None:
            wanted['ui.sqlMode'] = intent
        return self.journal.result('controls: ' + name, wanted, observed)

    # ---- contracts ----
    def seed_range(self, snapshot):
        tx = snapshot.get('transmit', {})
        maximum = tx.get('speechProcLevelMaximum')
        if type(maximum) is not int:
            self.journal.event('contract-gap', contract='range',
                               reason='snapshot publishes no speechProcLevelMaximum')
            return
        old = tx.get('speechProcLevel')
        seeds = range_seeds(maximum)
        seed = seeds[1] if old == seeds[0] else seeds[0]
        self.baseline.update({'transmit.speechProc': tx.get('speechProc'), 'transmit.speechProcLevel': old})
        self.journal.event('range-baseline', maximum=maximum, old=old, seed=seed)
        if tx.get('speechProc') is not True:
            # The radio writes the level only while the processor is on (Icom 14 0E).
            self.set_widget(PROC_ENABLE, 'setChecked', True, 'enable processor for its level')
        self.set_widget(PROC_LEVEL, 'setValue', seed, 'seed processor level')
        self.expected.update({'transmit.speechProc': True, 'transmit.speechProcLevel': seed})
        self.observe('range: processor level held', self.expected_for('transmit.'),
                     {PROC_LEVEL: ('setValue', seed)})

    def boundary_sql(self, snapshot):
        item = snapshot['slices'][0]
        self.baseline.update({'slice.squelch': item.get('squelch'), 'slice.squelchLevel': item.get('squelchLevel'),
                              'slice.manualSquelchLevel': item.get('manualSquelchLevel')})
        self.sql_baseline = sql_mode(self.tree())
        self.journal.event('sql-baseline', mode=self.sql_baseline, slice=item)
        if self.sql_baseline is None:
            raise StopRun('SQL surface is missing or ambiguous')
        self.select_sql('manual')
        self.set_widget(SQL_LEVEL, 'setValue', 0, 'manual SQL at threshold 0')
        self.observe('boundary: manual SQL at 0 stays on',
                     {'slice.squelch': True, 'slice.squelchLevel': 0},
                     {SQL_LEVEL: ('setValue', 0)}, intent='manual')

    def seed_sql(self):
        """Re-enter Manual first: the boundary contract may have left SQL Off.

        Failing to re-enter it is the #6172 trap itself (Manual at a remembered
        0 reads back Off), so it is a recorded concern, not a stop."""
        try:
            self.select_sql('manual')
        except StopRun as exc:
            self.journal.data['results'].append({
                'scenario': 'controls: restart: manual SQL level held', 'outcome': 'CONCERN',
                'reason': f'SQL Manual could not be re-entered after the boundary contract: {exc}'})
            self.journal.save()
            print('controls: restart: manual SQL level held: CONCERN', flush=True)
            return
        seed = SQL_SEEDS[1] if self.baseline.get('slice.squelchLevel') == SQL_SEEDS[0] else SQL_SEEDS[0]
        self.set_widget(SQL_LEVEL, 'setValue', seed, 'distinctive manual SQL level')
        self.expected.update({'slice.squelch': True, 'slice.squelchLevel': seed})
        self.observe('restart: manual SQL level held', self.expected_for('slice.'),
                     {SQL_LEVEL: ('setValue', seed)}, intent='manual')

    def expected_for(self, prefix):
        return {k: v for k, v in self.expected.items() if k.startswith(prefix)}

    def restart(self):
        self.supervisor.quit()
        self.supervisor.launch()
        self.connect()
        # Observe before anything could repair it: the connect read is the writer.
        self.observe('full client restart', dict(self.expected))

    def restore(self):
        current = state(self.snapshot())
        conflicts = {k: current.get(k) for k, v in self.expected.items() if current.get(k) != v}
        if conflicts:
            self.journal.data['cleanup'].append({'outcome': 'INCONCLUSIVE', 'conflicts': conflicts,
                'reason': 'state differs from the last test intent; left intact for operator review'})
            self.journal.save()
            return
        restored = {}
        if 'transmit.speechProcLevel' in self.expected and self.baseline.get('transmit.speechProcLevel') is not None:
            self.set_widget(PROC_LEVEL, 'setValue', self.baseline['transmit.speechProcLevel'], 'restore processor level')
            restored['transmit.speechProcLevel'] = self.baseline['transmit.speechProcLevel']
        if self.baseline.get('transmit.speechProc') is False:
            self.set_widget(PROC_ENABLE, 'setChecked', False, 'restore processor enable')
            restored['transmit.speechProc'] = False
        sql_restored = None
        if self.sql_baseline is not None:
            level = self.baseline.get('slice.manualSquelchLevel')
            try:
                if type(level) is int and sql_mode(self.tree()) == 'manual':
                    self.set_widget(SQL_LEVEL, 'setValue', level, 'restore manual SQL level')
                self.select_sql(self.sql_baseline)
                sql_restored = True
            except StopRun as exc:
                # The #6172 trap can make Manual unreachable; leave it for review.
                sql_restored = f'INCONCLUSIVE: {exc}'
        result = self.observe('cleanup', restored, samples=4) if restored else {'outcome': 'NOT RUN'}
        self.journal.data['cleanup'].append({**result, 'sqlMode': self.sql_baseline,
                                             'sqlRestored': sql_restored})
        self.journal.save()

    def execute(self):
        snapshot = self.connect()
        self.seed_range(snapshot)
        snapshot = self.snapshot()
        self.boundary_sql(snapshot)
        self.seed_sql()
        self.restart()
        self.restore()


def main(argv=None):
    parser = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    parser.add_argument('command', choices=('plan', 'run'))
    parser.add_argument('--app', type=Path)
    parser.add_argument('--profile', type=Path, help='new, dedicated directory; must not exist')
    parser.add_argument('--output', type=Path, help='new evidence directory; must not exist')
    target = parser.add_mutually_exclusive_group()
    target.add_argument('--serial', help='exact discovery serial (Flex, HL2)')
    target.add_argument('--icom-host', help='Icom network host; sign in through the client window')
    parser.add_argument('--sign-in-seconds', type=int, default=300)
    args = parser.parse_args(argv)
    if args.command == 'plan':
        print(json.dumps(plan(), indent=2))
        return 0
    if sys.platform == 'win32':
        parser.error('process supervision currently supports macOS/Linux')
    if not all((args.app, args.profile, args.output)) or not (args.serial or args.icom_host):
        parser.error('run requires --app --profile --output and --serial or --icom-host')
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
                      'sha256': hashlib.sha256(app.read_bytes()).hexdigest(),
                      'target': {'serial': args.serial, 'icomHost': args.icom_host}})
    sources = {}
    for name in ('radiocert_persist_controls.py', 'radiocert_persist.py', 'radiocert_persist_applets.py'):
        source = Path(__file__).with_name(name).read_bytes()
        sources[name] = hashlib.sha256(source).hexdigest()
        (output / name).write_bytes(source)
    journal.data['metadata']['runnerSources'] = sources
    journal.save()
    supervisor = Supervisor(app, profile, output, journal)
    try:
        supervisor.initialize_profile()
        supervisor.launch()
        ControlsRun(supervisor, journal, serial=args.serial, icom_host=args.icom_host,
                    sign_in_seconds=args.sign_in_seconds).execute()
        journal.event('client-left-open', pid=supervisor.process.pid)
        journal.data['status'] = 'completed'
    except (StopRun, OSError, ConnectionError, KeyError, ValueError, subprocess.TimeoutExpired, KeyboardInterrupt) as exc:
        journal.data['status'] = 'interrupted'
        journal.event('stopped', reason=str(exc), recovery='Inspect action-pending and baselines; no blind restore.')
        print(f'Stopped: {exc}', file=sys.stderr, flush=True)
        return 2
    finally:
        journal.save()
        journal.report()
    print(f'Evidence: {journal.path}')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
