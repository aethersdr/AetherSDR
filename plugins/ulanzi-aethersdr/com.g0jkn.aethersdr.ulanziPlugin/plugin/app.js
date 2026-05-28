// AetherSDR Controller — Ulanzi Studio plugin
//
// Bridges Ulanzi Studio (which manages the D100H dial / LCD button device
// over Bluetooth) to AetherSDR (the desktop SDR app) via TCI WebSocket on
// port 40001.  Studio sends us button-press + dial events; we translate
// them into TCI commands and ship them to AetherSDR.
//
// TCI command vocabulary in this plugin is faithful to the existing
// AetherSDR Stream Deck plugin (com.aethersdr.radio at port 40001) so
// the two plugins behave identically against the same radio.

import UlanziApi from '../libs/common-node/index.js';
import WebSocket from 'ws';

// ─── State ───────────────────────────────────────────────────────────────

const PLUGIN_UUID = 'com.g0jkn.aethersdr.controller';

// AetherSDR's TCI WebSocket server.  Default is the same port the Elgato
// AetherSDR plugin uses (40001) — confirmed live against the same radio.
const DEFAULT_TCI_URL = 'ws://127.0.0.1:40001';

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
  sliceIndex: 0,           // 0..7 (A..H) — incremented locally for "slice cycle"
  transmitting: false,
  tuning: false,
  muted: false,
  volume: 50,
  rfPower: 100,
  tunePower: 25,
  micLevel: 50,            // best-effort tracker — TCI 'mic_level' verb is non-standard
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

  tci.on('open',    () => { tciReady = true;  console.log('[tci] connected'); });
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
      // TCI spec format: `verb:<trx>,<value>;` — p[0] is the receiver index
      // (always 0 for the active slice), p[1] is the actual value.  Reading
      // p[0] would always give "0" → ±5 steps would snap to 5 / 0 forever.
      // (The Elgato AetherSDR plugin has the same bug; it's masked there
      // because RF Power cycles through a preset array rather than relative
      // steps.)
      case 'volume':       if (p.length >= 2) radio.volume       = parseInt(p[1]);       break;
      case 'drive':        if (p.length >= 2) radio.rfPower      = parseInt(p[1]);       break;
      case 'mic_level':    if (p.length >= 2) radio.micLevel     = parseInt(p[1]);       break;
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

function cmdSliceCycle() {
  // TCI does not expose a clean "next slice" — local-increment for now.
  // TODO: confirm correct slice-focus command against AetherSDR's TCI spec.
  radio.sliceIndex = (radio.sliceIndex + 1) % 8;
  return `if:${radio.sliceIndex};`;
}

function cmdVfoStep(direction)       { return cmdSetFreq(radio.frequency + direction * TX_STEP_HZ); }
function cmdVfoStepCoarse(direction) { return cmdSetFreq(radio.frequency + direction * TX_STEP_HZ * COARSE_MULT); }

// Gain ±5 helpers — clamp to 0–100 so we don't blow past the radio's range.
// Format `verb:<trx>,<value>;` matches the TCI spec (ExpertSDR2 1.9) and
// what AetherSDR emits, so parser + sender stay symmetric.  `mic_level` is
// best-effort — AetherSDR's TCI handler may silently ignore it since it's
// not in the published spec; we send anyway and trust the log if the value
// never changes.
const clamp01_100 = (v) => Math.max(0, Math.min(100, v));
function cmdAfGain(direction)   { return `volume:0,${clamp01_100(radio.volume   + direction * GAIN_STEP)};`; }
function cmdRfGain(direction)   { return `drive:0,${clamp01_100(radio.rfPower  + direction * GAIN_STEP)};`; }
function cmdMicGain(direction)  { return `mic_level:0,${clamp01_100(radio.micLevel + direction * GAIN_STEP)};`; }

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
  if (jsn.param && jsn.param.tci_url && jsn.param.tci_url !== tciUrl) tciConnect(jsn.param.tci_url);
});

$UD.onClear((jsn) => {
  if (!jsn.param) return;
  for (const item of jsn.param) delete ACTION_CACHES[item.context];
});

$UD.onParamFromPlugin((jsn) => {
  if (ACTION_CACHES[jsn.context]) ACTION_CACHES[jsn.context].settings = jsn.param || {};
  if (jsn.param && jsn.param.tci_url && jsn.param.tci_url !== tciUrl) tciConnect(jsn.param.tci_url);
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
    case `${PLUGIN_UUID}.sliceCycle`:  tciSend(cmdSliceCycle());  break;
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

// Encoder (D100H dial) — VFO action handles all 5 dial events.
$UD.onDialRotateRight(()     => tciSend(cmdVfoStep(+1)));
$UD.onDialRotateLeft(()      => tciSend(cmdVfoStep(-1)));
$UD.onDialRotateHoldRight(() => tciSend(cmdVfoStepCoarse(+1)));
$UD.onDialRotateHoldLeft(()  => tciSend(cmdVfoStepCoarse(-1)));
$UD.onDialDown(()            => tciSend(cmdMoxToggle()));
$UD.onDialUp(()              => {});

// ─── Crash hooks ─────────────────────────────────────────────────────────
process.on('unhandledRejection', (err) => console.error('[unhandled]', err));
process.on('uncaughtException',  (err) => console.error('[crash]', err));
