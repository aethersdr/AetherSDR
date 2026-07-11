// AetherSDR Controller — Ulanzi Studio plugin
//
// Bridges Ulanzi Studio (which manages the D100H dial / LCD button device
// over Bluetooth) to AetherSDR (the desktop SDR app) via TCI WebSocket on
// port 50001.  Studio sends us button-press + dial events; we translate
// them into TCI commands and ship them to AetherSDR.
//
// TCI command vocabulary in this plugin is faithful to the existing
// AetherSDR Stream Deck plugin (com.aethersdr.radio at port 50001) so
// the two plugins behave identically against the same radio.

import UlanziApi from '../libs/common-node/index.js';
import WebSocket from 'ws';

// ─── State ───────────────────────────────────────────────────────────────

const PLUGIN_UUID = 'com.g0jkn.aethersdr.controller';

// AetherSDR's TCI WebSocket server.  Default is the same port the Elgato
// AetherSDR plugin uses (50001) — confirmed live against the same radio.
const DEFAULT_TCI_URL = 'ws://127.0.0.1:50001';

// The plugin shipped for a while with a wrong default port of 40001 (#4154),
// which the inspector persisted into per-action settings for anyone who edited
// the URL field. 40001 was never a working AetherSDR port, so a saved 40001 is
// always the stale default, not a deliberate choice — upgrade it to 50001 so
// existing installs connect after updating instead of silently retrying a dead
// port. Any other host/port the operator set is left untouched.
function migrateTciUrl(url) {
  if (typeof url !== 'string') return url;
  return url.replace(/^(ws:\/\/(?:127\.0\.0\.1|localhost)):40001\b/i, '$1:50001');
}

const ACTION_CACHES = {};
let tci = null;
let tciUrl = DEFAULT_TCI_URL;
let tciReady = false;
let reconnectTimer = 0;

// Live radio state — populated from incoming TCI messages.  Toggles read
// the current value before flipping it (mirrors the Stream Deck plugin's
// approach since TCI doesn't have a server-side "toggle" verb).
const radio = {
  frequency: 14225000,
  mode: 'USB',
  transmitting: false,
  tuning: false,
  muted: false,
  volume: 50,
  rfPower: 100,
  tunePower: 25,
  micLevel: 50,            // best-effort tracker — TCI 'mic_level' verb is non-standard
  sqlLevel: 0,             // squelch threshold (TCI 'sql_level', 0–100)
  filterLow: 100,          // RX filter low edge (Hz) — SSB-ish default until first echo
  filterHigh: 2800,        // RX filter high edge (Hz)
  nbOn: false, nrOn: false, anfOn: false, apfOn: false,
  sqlOn: false, split: false, locked: false,
  ritOn: false, xitOn: false,
};

// Band centres for "band up" / "band down".  Matches the Stream Deck
// plugin so the two plugins navigate the same way.
const BANDS = {
  '160m': 1900000,  '80m': 3800000,  '60m': 5357000,  '40m': 7200000,
  '30m': 10125000,  '20m': 14225000, '17m': 18118000, '15m': 21300000,
  '12m': 24940000,  '10m': 28400000, '6m':  50125000,
};
const BAND_ORDER = Object.keys(BANDS);

// Mode cycle order — USB → LSB → CW → DIGU → DIGL → AM → FM → loop.
const MODE_CYCLE = ['USB', 'LSB', 'CW', 'DIGU', 'DIGL', 'AM', 'FM'];

function closestBandIndex(freq) {
  let best = 0, bestDist = Infinity;
  for (let i = 0; i < BAND_ORDER.length; i++) {
    const dist = Math.abs(freq - BANDS[BAND_ORDER[i]]);
    if (dist < bestDist) { bestDist = dist; best = i; }
  }
  return best;
}

// ─── TCI WebSocket ───────────────────────────────────────────────────────

function tciConnect(url) {
  tciUrl = url || tciUrl;
  if (tci) {
    try { tci.removeAllListeners(); tci.close(); } catch (_) {}
    tci = null;
  }
  console.log(`[tci] connecting to ${tciUrl}`);
  tci = new WebSocket(tciUrl);

  tci.on('open',    () => {
    tciReady = true;
    console.log('[tci] connected');
    // Seed the local gain mirrors from AE's actual current values. A bare verb
    // is a GET; AE replies `volume:<v>;`/`drive:<v>;`/`mic_level:<v>;` which
    // parseTci folds into radio.{volume,rfPower,micLevel}. Without this the
    // mirror sits at the hard-coded defaults (50/100/50) until the first init
    // burst arrives — and AE's init burst doesn't always carry all three — so
    // the first ±5 press would jump from the default instead of the real value.
    tciSend('volume;');
    tciSend('drive;');
    tciSend('mic_level;');
    // Same rationale for the new dial controls — seed from AE's real values so
    // the first detent steps from the live filter/squelch state, not a guess.
    tciSend('rx_filter_band:0;');
    tciSend('sql_level:0;');
  });
  tci.on('close',   () => { tciReady = false; console.log('[tci] closed — retry in 5s'); scheduleReconnect(); });
  tci.on('error',   (err) => console.log(`[tci] error: ${err.message}`));
  tci.on('message', (data) => parseTci(data.toString()));
}

function scheduleReconnect() {
  if (reconnectTimer) return;
  reconnectTimer = setTimeout(() => { reconnectTimer = 0; tciConnect(); }, 5000);
}

function tciSend(cmd) {
  if (!tci || !tciReady) {
    console.log(`[tci] DROPPED (not connected): ${cmd}`);
    return false;
  }
  console.log(`[tci] -> ${cmd}`);
  tci.send(cmd);
  return true;
}

// Parse incoming TCI messages so toggles know the current state.  Trust
// matrix taken from the Elgato plugin's parseTci — the subset we need
// to handle our 8 actions.
function parseTci(msg) {
  for (const line of msg.split('\n')) {
    const t = line.trim().replace(/;$/, '');
    if (!t) continue;
    const ci = t.indexOf(':');
    if (ci < 0) continue;
    const cmd = t.substring(0, ci).toLowerCase();
    const p = t.substring(ci + 1).split(',');
    switch (cmd) {
      case 'vfo':          if (p.length >= 3) radio.frequency    = parseInt(p[2]);       break;
      case 'modulation':   if (p.length >= 2) radio.mode         = p[1];                 break;
      case 'trx':          if (p.length >= 2) radio.transmitting = p[1] === 'true';      break;
      case 'tune':         if (p.length >= 1) radio.tuning       = p[0] === 'true';      break;
      case 'rit_enable':   if (p.length >= 2) radio.ritOn        = p[1] === 'true';      break;
      // Gain / level trackers — keep local mirror in sync so ±5 steps are
      // calculated against the radio's actual current value, not a stale guess.
      //
      // AetherSDR has **asymmetric** emit formats for these verbs:
      //   - Init burst (TCI connect)  : `verb:<trx>,<value>;`  — two params
      //   - Steady-state value change : `verb:<value>;`        — single param
      //
      // Verified live 2026-05-27 via TCI Monitor: pressing AF Gain ▲ sends
      // `volume:0,55;` and AE responds with `volume:55;` (no trx prefix).
      // So our parser must accept BOTH lengths — read p[1] when trx-prefixed,
      // otherwise p[0].  Earlier versions only handled the two-param case
      // and dropped every steady-state update, freezing the local mirror at
      // the init-burst snapshot → ±5 steps bounced ±5 around that frozen
      // value forever (e.g. 45 ↔ 55 around an init volume of 50).
      case 'volume':       radio.volume   = parseInt(p.length >= 2 ? p[1] : p[0]); break;
      case 'drive':        radio.rfPower  = parseInt(p.length >= 2 ? p[1] : p[0]); break;
      case 'mic_level':    radio.micLevel = parseInt(p.length >= 2 ? p[1] : p[0]); break;
      // Dial controls. AE emits `rx_filter_band:<trx>,<low>,<high>;` and
      // `sql_level:<trx>,<level>;` — both trx-prefixed, so read past p[0].
      case 'rx_filter_band':
        if (p.length >= 3) { radio.filterLow = parseInt(p[1]); radio.filterHigh = parseInt(p[2]); }
        break;
      case 'sql_level':    radio.sqlLevel = parseInt(p.length >= 2 ? p[1] : p[0]); break;
    }
  }
}

// ─── TCI command builders ────────────────────────────────────────────────

const TX_STEP_HZ      = 100;     // VFO rotate CW/CCW step
const COARSE_MULT     = 10;      // press+rotate is ×10 the step
const GAIN_STEP       = 5;       // ±5 per press for AF / RF / mic gain (range 0–100)

function cmdMoxToggle()       { return `trx:0,${!radio.transmitting};`; }
function cmdTuneToggle()      { return `tune:0,${!radio.tuning};`; }
function cmdRitToggle()       { return `rit_enable:0,${!radio.ritOn};`; }
function cmdSetFreq(hz)       { return `vfo:0,0,${hz};`; }
function cmdSetMode(mode)     { return `modulation:0,${mode};`; }

function cmdModeNext() {
  const i = MODE_CYCLE.indexOf(radio.mode);
  const next = MODE_CYCLE[(i + 1) % MODE_CYCLE.length];
  return cmdSetMode(next);
}

function cmdBandUp() {
  const i = Math.min(closestBandIndex(radio.frequency) + 1, BAND_ORDER.length - 1);
  return cmdSetFreq(BANDS[BAND_ORDER[i]]);
}

function cmdBandDown() {
  const i = Math.max(closestBandIndex(radio.frequency) - 1, 0);
  return cmdSetFreq(BANDS[BAND_ORDER[i]]);
}

function cmdVfoStep(direction)       { return cmdSetFreq(radio.frequency + direction * TX_STEP_HZ); }
function cmdVfoStepCoarse(direction) { return cmdSetFreq(radio.frequency + direction * TX_STEP_HZ * COARSE_MULT); }

// Gain ±5 helpers — clamp to 0–100 so we don't blow past the radio's range.
// Format `verb:<trx>,<value>;` matches the TCI spec and AE accepts it.
//
// Optimistic local-mirror update: AetherSDR only emits `drive:` over TCI
// at init-burst time — value changes after that are silent.  Verified via
// TCI Monitor 2026-05-27 (Documents/tci-monitor-20260527-210149.log lines
// 49-61: ▲▲▲▲▲▼▼▼▼▲▲▲▲ all sent `drive:0,10` or `drive:0,0`, zero `◀ drive`
// echoes from AE).  If we waited for an echo to update radio.rfPower, ±5
// steps would always compute against the stale init-burst snapshot —
// bouncing between init±5 forever.  So we update the mirror BEFORE sending,
// optimistically assuming AE accepts.  If AE rejects (clamp, lock, etc.),
// the parser still catches any later echo and corrects us.
//
// AF volume is echoed (so parser tracking works there too), but doing the
// optimistic update for it as well keeps the three actions consistent and
// is harmless — the subsequent parser echo just confirms the same value.
//
// `mic_level` is an AE extension (not in the published TCI spec) but is
// honoured — verified live 2026-05-28 to drive TX mic gain on AetherSDR.
const clamp01_100 = (v) => Math.max(0, Math.min(100, v));

// `step` defaults to the keypad's ±5; the dial handlers pass their own
// per-detent step (see DIAL below).
function cmdAfGain(direction, step = GAIN_STEP) {
  const v = clamp01_100(radio.volume + direction * step);
  radio.volume = v;
  return `volume:0,${v};`;
}
function cmdRfGain(direction, step = GAIN_STEP) {
  const v = clamp01_100(radio.rfPower + direction * step);
  radio.rfPower = v;
  return `drive:0,${v};`;
}
function cmdMicGain(direction, step = GAIN_STEP) {
  const v = clamp01_100(radio.micLevel + direction * step);
  radio.micLevel = v;
  return `mic_level:0,${v};`;
}

// ── Dial-only controls ─────────────────────────────────────────────────────
// Per-detent steps for the rotary (Encoder) actions. Fine = single detent,
// coarse = press-and-rotate. Gains/squelch are 0–100; filter width is in Hz.
const DIAL = {
  gainFine: 2,  gainCoarse: 10,
  sqlFine:  2,  sqlCoarse:  10,
  bwFine:   50, bwCoarse:   250,   // total passband-width change per detent (Hz)
};
const FILTER_MIN_WIDTH = 50;
const FILTER_MAX_WIDTH = 8000;

function cmdSqlLevel(direction, step) {
  const v = clamp01_100(radio.sqlLevel + direction * step);
  radio.sqlLevel = v;
  return `sql_level:0,${v};`;
}

// Widen/narrow the passband symmetrically about its current centre. Keeps the
// voice/CW pitch centred while the operator opens or closes the filter.
function cmdFilterWidth(direction, step) {
  const center = (radio.filterLow + radio.filterHigh) / 2;
  let width = (radio.filterHigh - radio.filterLow) + direction * step;
  width = Math.max(FILTER_MIN_WIDTH, Math.min(FILTER_MAX_WIDTH, width));
  const lo = Math.round(center - width / 2);
  const hi = Math.round(center + width / 2);
  radio.filterLow = lo;
  radio.filterHigh = hi;
  return `rx_filter_band:0,${lo},${hi};`;
}

// ─── Studio API ──────────────────────────────────────────────────────────

const $UD = new UlanziApi();
$UD.connect(PLUGIN_UUID);

$UD.onConnected(() => {
  console.log(`[studio] connected as ${PLUGIN_UUID}`);
  tciConnect();
});

$UD.onClose(() => console.log('[studio] disconnected'));
$UD.onError((err) => console.log(`[studio] error: ${err}`));

$UD.onAdd((jsn) => {
  // Studio's add event uses `uuid` (action CLASS) + `actionid` (instance);
  // not `action` like I originally assumed.  Store the class UUID so the
  // dispatch switch below can match against `${PLUGIN_UUID}.mox` etc.
  ACTION_CACHES[jsn.context] = { actionId: jsn.uuid, settings: jsn.param || {} };
  if (jsn.param && jsn.param.tci_url) {
    const url = migrateTciUrl(jsn.param.tci_url);
    if (url !== tciUrl) tciConnect(url);
  }
});

$UD.onClear((jsn) => {
  if (!jsn.param) return;
  for (const item of jsn.param) delete ACTION_CACHES[item.context];
});

$UD.onParamFromPlugin((jsn) => {
  if (ACTION_CACHES[jsn.context]) ACTION_CACHES[jsn.context].settings = jsn.param || {};
  if (jsn.param && jsn.param.tci_url) {
    const url = migrateTciUrl(jsn.param.tci_url);
    if (url !== tciUrl) tciConnect(url);
  }
});

// Keypad — button press.  Studio sends cmd:'keydown' (not cmd:'run');
// the SDK's onKeyDown is the right hook.  jsn.uuid carries the action
// class UUID we need to dispatch on; the SDK pre-resolves jsn.context
// for the per-key cache lookup.
$UD.onKeyDown((jsn) => {
  const cache = ACTION_CACHES[jsn.context];
  if (!cache) {
    console.log(`[keydown] no cache for context=${jsn.context} uuid=${jsn.uuid}`);
    return;
  }
  console.log(`[keydown] ${cache.actionId}`);
  switch (cache.actionId) {
    case `${PLUGIN_UUID}.mox`:         tciSend(cmdMoxToggle());   break;
    case `${PLUGIN_UUID}.tune`:        tciSend(cmdTuneToggle());  break;
    case `${PLUGIN_UUID}.modeCycle`:   tciSend(cmdModeNext());    break;
    case `${PLUGIN_UUID}.bandUp`:      tciSend(cmdBandUp());      break;
    case `${PLUGIN_UUID}.bandDown`:    tciSend(cmdBandDown());    break;
    case `${PLUGIN_UUID}.ritToggle`:   tciSend(cmdRitToggle());   break;
    // Direct-mode actions — for D200H pages that prefer explicit keys over cycling.
    case `${PLUGIN_UUID}.modeUsb`:     tciSend(cmdSetMode('USB'));  break;
    case `${PLUGIN_UUID}.modeLsb`:     tciSend(cmdSetMode('LSB'));  break;
    case `${PLUGIN_UUID}.modeCw`:      tciSend(cmdSetMode('CW'));   break;
    case `${PLUGIN_UUID}.modeDigu`:    tciSend(cmdSetMode('DIGU')); break;
    // Gain trio — each press = ±5; relative to currently-tracked value.
    case `${PLUGIN_UUID}.afGainUp`:    tciSend(cmdAfGain(+1));  break;
    case `${PLUGIN_UUID}.afGainDown`:  tciSend(cmdAfGain(-1));  break;
    case `${PLUGIN_UUID}.rfGainUp`:    tciSend(cmdRfGain(+1));  break;
    case `${PLUGIN_UUID}.rfGainDown`:  tciSend(cmdRfGain(-1));  break;
    case `${PLUGIN_UUID}.micGainUp`:   tciSend(cmdMicGain(+1)); break;
    case `${PLUGIN_UUID}.micGainDown`: tciSend(cmdMicGain(-1)); break;
    default:
      console.log(`[run] unhandled action: ${cache.actionId}`);
  }
});

// Encoder (dial) dispatch — action-aware.
//
// Studio stamps every event (including dial events) with jsn.context
// (uuid___key___actionid) and onAdd caches the action-class UUID for that
// context.  So we route each dial by *which Encoder action* it's bound to,
// exactly like onKeyDown routes keys.  A dial whose context isn't cached
// (legacy single-dial layout, or an event that beats onAdd) falls back to
// VFO tuning for rotate — harmless — but to NO-OP for press, so an
// unrecognised dial can never key the radio.
function dialActionId(jsn) {
  const cache = ACTION_CACHES[jsn.context];
  return cache ? cache.actionId : null;
}

function onDialTurn(jsn, dir, coarse) {
  switch (dialActionId(jsn)) {
    case `${PLUGIN_UUID}.afGain`:
      tciSend(cmdAfGain(dir, coarse ? DIAL.gainCoarse : DIAL.gainFine)); break;
    case `${PLUGIN_UUID}.rfGain`:
      tciSend(cmdRfGain(dir, coarse ? DIAL.gainCoarse : DIAL.gainFine)); break;
    case `${PLUGIN_UUID}.micGain`:
      tciSend(cmdMicGain(dir, coarse ? DIAL.gainCoarse : DIAL.gainFine)); break;
    case `${PLUGIN_UUID}.squelch`:
      tciSend(cmdSqlLevel(dir, coarse ? DIAL.sqlCoarse : DIAL.sqlFine)); break;
    case `${PLUGIN_UUID}.filterWidth`:
      tciSend(cmdFilterWidth(dir, coarse ? DIAL.bwCoarse : DIAL.bwFine)); break;
    case `${PLUGIN_UUID}.vfo`:
    default:  // VFO action, or legacy/unbound dial → tune (harmless)
      tciSend(coarse ? cmdVfoStepCoarse(dir) : cmdVfoStep(dir));
  }
}

$UD.onDialRotateRight(jsn     => onDialTurn(jsn, +1, false));
$UD.onDialRotateLeft(jsn      => onDialTurn(jsn, -1, false));
$UD.onDialRotateHoldRight(jsn => onDialTurn(jsn, +1, true));
$UD.onDialRotateHoldLeft(jsn  => onDialTurn(jsn, -1, true));

// Dial press — ONLY the VFO dial toggles MOX.  Every other dial (and any
// unrecognised/uncached context) is a deliberate no-op so a level-knob press
// can never accidentally key the transmitter.
$UD.onDialDown(jsn => {
  if (dialActionId(jsn) === `${PLUGIN_UUID}.vfo`) tciSend(cmdMoxToggle());
});
$UD.onDialUp(() => {});

// ─── Crash hooks ─────────────────────────────────────────────────────────
process.on('unhandledRejection', (err) => console.error('[unhandled]', err));
process.on('uncaughtException',  (err) => console.error('[crash]', err));
