/**
 * AetherSDR Stream Deck Plugin
 * Controls FlexRadio via AetherSDR's TCI WebSocket server.
 * Single-file, no build step, no npm dependencies.
 */

const WebSocket = require("ws");

// ── Parse Stream Deck launch arguments ──────────────────────────────────────
let sdPort, sdUUID, sdRegisterEvent, sdInfo;
for (let i = 0; i < process.argv.length; i++) {
    if (process.argv[i] === "-port")          sdPort = process.argv[i + 1];
    if (process.argv[i] === "-pluginUUID")    sdUUID = process.argv[i + 1];
    if (process.argv[i] === "-registerEvent") sdRegisterEvent = process.argv[i + 1];
    if (process.argv[i] === "-info")          sdInfo = JSON.parse(process.argv[i + 1]);
}

// ── Radio state ─────────────────────────────────────────────────────────────
const radio = {
    frequency: 14225000, mode: "USB",
    transmitting: false, tuning: false,
    muted: false, volume: 50,
    rfPower: 100, tunePower: 25,
    nbOn: false, nrOn: false, anfOn: false, apfOn: false,
    sqlOn: false, split: false, locked: false,
    ritOn: false, xitOn: false,
};

// ── TCI Client ──────────────────────────────────────────────────────────────
let tciWs = null;
let tciReconnectTimer = null;
const TCI_HOST = "localhost";
const TCI_PORT = 50001;

function tciConnect() {
    console.log(`Connecting to TCI at ws://${TCI_HOST}:${TCI_PORT}`);
    try {
        tciWs = new WebSocket(`ws://${TCI_HOST}:${TCI_PORT}`);
        tciWs.on("open", () => { console.log("TCI connected"); });
        tciWs.on("message", (data) => parseTci(data.toString()));
        tciWs.on("close", () => { tciWs = null; console.log("TCI disconnected, reconnecting in 3s"); tciScheduleReconnect(); });
        tciWs.on("error", (e) => console.log("TCI error:", e && e.message));
    } catch (e) { tciScheduleReconnect(); }
}

function tciScheduleReconnect() {
    if (tciReconnectTimer) return;
    tciReconnectTimer = setTimeout(() => { tciReconnectTimer = null; tciConnect(); }, 3000);
}

function tciSend(cmd) {
    if (tciWs && tciWs.readyState === WebSocket.OPEN) tciWs.send(cmd);
}

function parseTci(msg) {
    for (const line of msg.split("\n")) {
        const t = line.trim().replace(/;$/, "");
        if (!t) continue;
        const ci = t.indexOf(":");
        if (ci < 0) continue;
        const cmd = t.substring(0, ci).toLowerCase();
        const p = t.substring(ci + 1).split(",");
        switch (cmd) {
            case "vfo":          if (p.length >= 3) { radio.frequency = parseInt(p[2]); vfoDial.updateAll(); } break;
            case "modulation":   if (p.length >= 2) radio.mode = p[1]; break;
            case "trx":          if (p.length >= 2) radio.transmitting = p[1] === "true"; break;
            case "tune":         if (p.length >= 2) radio.tuning = p[1] === "true"; break;
            case "mute":         if (p.length >= 2) radio.muted = p[1] === "true"; break;
            case "volume":       if (p.length >= 1) { radio.volume = volumePercentFromDb(parseInt(p[0])); volumeDial.updateAll(); } break;
            // drive/tune_drive arrive as `drive:<power>;` (1 arg) when echoing our
            // own SET, but as `drive:<trx>,<power>;` (2 args) in the init burst —
            // the power is always the LAST field, matching TciProtocol::cmdDrive's
            // own `args[args.size() == 1 ? 0 : 1]` parsing.
            case "drive":        if (p.length >= 1) { radio.rfPower = parseInt(p[p.length - 1]); rfPowerDial.updateAll(); } break;
            case "tune_drive":   if (p.length >= 1) radio.tunePower = parseInt(p[p.length - 1]); break;
            case "rx_nb_enable":  if (p.length >= 2) radio.nbOn = p[1] === "true"; break;
            case "rx_nr_enable":  if (p.length >= 2) radio.nrOn = p[1] === "true"; break;
            case "rx_anf_enable": if (p.length >= 2) radio.anfOn = p[1] === "true"; break;
            case "rx_apf_enable": if (p.length >= 2) radio.apfOn = p[1] === "true"; break;
            case "sql_enable":   if (p.length >= 2) radio.sqlOn = p[1] === "true"; break;
            case "split_enable": if (p.length >= 2) radio.split = p[1] === "true"; break;
            case "lock":         if (p.length >= 2) radio.locked = p[1] === "true"; break;
            case "rit_enable":   if (p.length >= 2) radio.ritOn = p[1] === "true"; break;
            case "xit_enable":   if (p.length >= 2) radio.xitOn = p[1] === "true"; break;
        }
    }
}

// ── Band data ───────────────────────────────────────────────────────────────
const BANDS = {
    "160m": 1900000, "80m": 3800000, "60m": 5357000, "40m": 7200000,
    "30m": 10125000, "20m": 14225000, "17m": 18118000, "15m": 21300000,
    "12m": 24940000, "10m": 28400000, "6m": 50125000,
};
const BAND_ORDER = Object.keys(BANDS);

function closestBandIndex(freq) {
    return closestIndex(BAND_ORDER.map(b => BANDS[b]), freq);
}

// ── Action handlers ─────────────────────────────────────────────────────────
const POWER_LEVELS = [5, 10, 25, 50, 75, 100];
const TUNE_LEVELS = [5, 10, 15, 25, 50];
const TUNE_STEP_HZ = 1000; // 1 kHz per press; matches streamcontroller-aethersdr

function nextInCycle(levels, current) {
    return levels.find(l => l > current) || levels[0];
}

function clamp(v, min, max) {
    return Math.min(max, Math.max(min, v));
}

function closestIndex(levels, value) {
    let best = 0, bestDist = Infinity;
    for (let i = 0; i < levels.length; i++) {
        const dist = Math.abs(levels[i] - value);
        if (dist < bestDist) { bestDist = dist; best = i; }
    }
    return best;
}

// ── Dial (Encoder) support ───────────────────────────────────────────────────
// Shared by every Stream Deck+ knob: each rotate/press is driven by caller-
// supplied nextValue()/onPress() rules, and mirrored back via setFeedback.
// Sending one TCI command per tick would flood the socket during a fast
// spin, so the outgoing command is debounced to one per quiet window while
// the local value (and on-dial feedback) updates immediately.
const DIAL_SEND_DEBOUNCE_MS = 25;

function createDial({ uuid, initialState, nextValue, onPress, getValue, setValue, sendCommand, formatTitle, formatValue }) {
    const instances = new Map(); // context -> per-instance mutable state (e.g. stepIndex)
    let sendTimer = null;

    function sendFeedback(context) {
        const state = instances.get(context);
        if (!state) return;
        sdWs.send(JSON.stringify({
            event: "setFeedback",
            context,
            payload: { title: formatTitle(state), value: formatValue(getValue()) },
        }));
    }

    return {
        uuid,
        updateAll() { for (const context of instances.keys()) sendFeedback(context); },
        willAppear(context) { instances.set(context, initialState()); sendFeedback(context); },
        willDisappear(context) { instances.delete(context); },
        dialRotate(context, ticks) {
            const state = instances.get(context);
            if (!state) return;
            setValue(nextValue(getValue(), ticks, state));
            sendFeedback(context);
            if (!sendTimer) sendTimer = setTimeout(() => { sendTimer = null; sendCommand(getValue()); }, DIAL_SEND_DEBOUNCE_MS);
        },
        dialDown(context) {
            const state = instances.get(context);
            if (!state) return;
            if (onPress) onPress(state);
            sendFeedback(context);
        },
    };
}

const VFO_TUNE_UUID = "com.aethersdr.radio.vfo-tune";
const RF_POWER_DIAL_UUID = "com.aethersdr.radio.rf-power-dial";
const VOLUME_DIAL_UUID = "com.aethersdr.radio.volume-dial";

function formatFreqMHz(hz) {
    return (hz / 1e6).toFixed(3);
}

function formatStepHz(hz) {
    if (hz >= 1000000) return `${hz / 1000000} MHz`;
    if (hz >= 1000) return `${hz / 1000} kHz`;
    return `${hz} Hz`;
}

// TCI's "volume" command is dB-scaled per the TCI v2.0 spec (-60..0 dB; 0 dB
// = full volume, -60 dB = silence) — see TciProtocol::cmdVolume
// (src/core/TciProtocol.cpp). SET accepts a plain-percent shorthand for
// values >= 1, but percent 0 is ambiguous with "0 dB = full volume", so true
// silence must be requested via the dB path explicitly (-60). Every
// broadcast/echo is always in dB, so it's converted back to percent to stay
// radio-authoritative (mirrors TciProtocol::volumePercentFromDb exactly).
function volumeSetCommand(pct) {
    return pct <= 0 ? "volume:-60;" : `volume:${pct};`;
}

function volumePercentFromDb(db) {
    if (db <= -60) return 0;
    if (db > 0) db = 0;
    const pct = Math.round(100 * Math.pow(10, db / 20));
    return Math.min(100, Math.max(1, pct));
}

const VFO_STEPS = [10, 100, 1000, 10000, 100000, 1000000]; // Hz
// 30 kHz floor / 50 GHz ceiling match the transverter-aware bounds used
// elsewhere for user-facing frequency entry (VfoWidget.cpp onXvtr path,
// SpectrumWidget.cpp spot-frequency spin) — a plain VHF/UHF cap would block
// operators running a transverter above the radio's native range.
const VFO_FREQ_MIN = 30000;
const VFO_FREQ_MAX = 50000000000;

const vfoDial = createDial({
    uuid: VFO_TUNE_UUID,
    initialState: () => ({ stepIndex: 2 }), // 1 kHz
    nextValue: (cur, ticks, state) => clamp(cur + ticks * VFO_STEPS[state.stepIndex], VFO_FREQ_MIN, VFO_FREQ_MAX),
    onPress: (state) => { state.stepIndex = (state.stepIndex + 1) % VFO_STEPS.length; },
    getValue: () => radio.frequency,
    setValue: (v) => { radio.frequency = v; },
    sendCommand: (hz) => tciSend(`vfo:0,0,${hz};`),
    formatTitle: (state) => `Step: ${formatStepHz(VFO_STEPS[state.stepIndex])}`,
    formatValue: (hz) => formatFreqMHz(hz),
});

const RF_POWER_STEPS = [1, 5];

const rfPowerDial = createDial({
    uuid: RF_POWER_DIAL_UUID,
    initialState: () => ({ stepIndex: 0 }),
    nextValue: (cur, ticks, state) => clamp(cur + ticks * RF_POWER_STEPS[state.stepIndex], 0, 100),
    onPress: (state) => { state.stepIndex = (state.stepIndex + 1) % RF_POWER_STEPS.length; },
    getValue: () => radio.rfPower,
    setValue: (v) => { radio.rfPower = v; },
    sendCommand: (w) => tciSend(`drive:${w};`),
    formatTitle: (state) => `Step: ${RF_POWER_STEPS[state.stepIndex]} W`,
    formatValue: (w) => `${w} W`,
});

// Only these percentages survive TCI's dB-quantized wire round-trip exactly
// (percent -> dB -> percent, see volumeSetCommand/volumePercentFromDb above);
// no uniform step avoids collisions above ~50% (e.g. 60% and 65% both
// reconstruct as 63%). Stepping across this curated, round-trip-safe subset
// keeps the dial glitch-free while staying fully radio-authoritative.
const VOLUME_LEVELS = [0, 5, 11, 18, 25, 32, 40, 50, 63, 79, 100];

const volumeDial = createDial({
    uuid: VOLUME_DIAL_UUID,
    initialState: () => ({}),
    nextValue: (cur, ticks) => VOLUME_LEVELS[clamp(closestIndex(VOLUME_LEVELS, cur) + ticks, 0, VOLUME_LEVELS.length - 1)],
    // TciServer excludes the sender from the command-echo broadcast (see
    // drive/rx_*_enable comment below), and "mute" has no dedicated
    // all-clients broadcast the way "volume" does — so this must be
    // optimistic, or a second press would never observe its own first press.
    onPress: () => { radio.muted = !radio.muted; tciSend(`mute:0,${radio.muted};`); },
    getValue: () => radio.volume,
    setValue: (v) => { radio.volume = v; },
    sendCommand: (v) => tciSend(volumeSetCommand(v)),
    formatTitle: () => "Push: Mute",
    formatValue: (v) => `${v}%`,
});

const DIALS = [vfoDial, rfPowerDial, volumeDial];
function dialFor(action) { return DIALS.find(d => d.uuid === action); }

const actionHandlers = {
    // TX
    "com.aethersdr.radio.ptt":         { keyDown: () => tciSend("trx:0,true;"),  keyUp: () => tciSend("trx:0,false;") },
    "com.aethersdr.radio.mox-toggle":  { keyDown: () => tciSend(`trx:0,${!radio.transmitting};`) },
    "com.aethersdr.radio.tune-toggle": { keyDown: () => tciSend(`tune:0,${!radio.tuning};`) },
    // drive/tune_drive/rx_*_enable have no dedicated all-clients broadcast
    // (see TciServer.cpp ~line 730: the command-echo broadcast explicitly
    // excludes the sender) unlike volume/lock, which do — so the sender
    // never hears its own change confirmed and must apply it optimistically,
    // or every next press would recompute from the same stale value.
    "com.aethersdr.radio.rf-power":    { keyDown: () => { radio.rfPower = nextInCycle(POWER_LEVELS, radio.rfPower); tciSend(`drive:${radio.rfPower};`); rfPowerDial.updateAll(); } },
    "com.aethersdr.radio.tune-power":  { keyDown: () => { radio.tunePower = nextInCycle(TUNE_LEVELS, radio.tunePower); tciSend(`tune_drive:${radio.tunePower};`); } },
    // Audio
    "com.aethersdr.radio.mute-toggle":  { keyDown: () => tciSend(`mute:0,${!radio.muted};`) },
    "com.aethersdr.radio.volume-up":    { keyDown: () => tciSend(volumeSetCommand(VOLUME_LEVELS[Math.min(closestIndex(VOLUME_LEVELS, radio.volume) + 1, VOLUME_LEVELS.length - 1)])) },
    "com.aethersdr.radio.volume-down":  { keyDown: () => tciSend(volumeSetCommand(VOLUME_LEVELS[Math.max(closestIndex(VOLUME_LEVELS, radio.volume) - 1, 0)])) },
    // DSP
    "com.aethersdr.radio.nb-toggle":  { keyDown: () => { radio.nbOn = !radio.nbOn; tciSend(`rx_nb_enable:0,${radio.nbOn};`); } },
    "com.aethersdr.radio.nr-toggle":  { keyDown: () => { radio.nrOn = !radio.nrOn; tciSend(`rx_nr_enable:0,${radio.nrOn};`); } },
    "com.aethersdr.radio.anf-toggle": { keyDown: () => { radio.anfOn = !radio.anfOn; tciSend(`rx_anf_enable:0,${radio.anfOn};`); } },
    "com.aethersdr.radio.apf-toggle": { keyDown: () => { radio.apfOn = !radio.apfOn; tciSend(`rx_apf_enable:0,${radio.apfOn};`); } },
    "com.aethersdr.radio.sql-toggle": { keyDown: () => tciSend(`sql_enable:0,${!radio.sqlOn};`) },
    // Slice
    "com.aethersdr.radio.split-toggle": { keyDown: () => tciSend(`split_enable:0,${!radio.split};`) },
    "com.aethersdr.radio.lock-toggle":  { keyDown: () => tciSend(`lock:0,${!radio.locked};`) },
    "com.aethersdr.radio.rit-toggle":   { keyDown: () => tciSend(`rit_enable:0,${!radio.ritOn};`) },
    "com.aethersdr.radio.xit-toggle":   { keyDown: () => tciSend(`xit_enable:0,${!radio.xitOn};`) },
    // Frequency
    "com.aethersdr.radio.tune-up":   { keyDown: () => tciSend(`vfo:0,0,${radio.frequency + TUNE_STEP_HZ};`) },
    "com.aethersdr.radio.tune-down": { keyDown: () => tciSend(`vfo:0,0,${radio.frequency - TUNE_STEP_HZ};`) },
    "com.aethersdr.radio.band-up":   { keyDown: () => { const i = Math.min(closestBandIndex(radio.frequency) + 1, BAND_ORDER.length - 1); tciSend(`vfo:0,0,${BANDS[BAND_ORDER[i]]};`); } },
    "com.aethersdr.radio.band-down": { keyDown: () => { const i = Math.max(closestBandIndex(radio.frequency) - 1, 0); tciSend(`vfo:0,0,${BANDS[BAND_ORDER[i]]};`); } },
    // DVK
    "com.aethersdr.radio.dvk-play":   { keyDown: () => tciSend("rx_play:0,true;") },
    "com.aethersdr.radio.dvk-record": { keyDown: () => tciSend("rx_record:0,true;") },
};

// Band actions
for (const [band, freq] of Object.entries(BANDS)) {
    actionHandlers[`com.aethersdr.radio.band-${band}`] = { keyDown: () => tciSend(`vfo:0,0,${freq};`) };
}

// Mode actions
for (const mode of ["USB", "LSB", "CW", "AM", "FM", "DIGU", "DIGL", "FT8"]) {
    actionHandlers[`com.aethersdr.radio.mode-${mode.toLowerCase()}`] = { keyDown: () => tciSend(`modulation:0,${mode};`) };
}

// ── Stream Deck WebSocket connection ────────────────────────────────────────
const sdWs = new WebSocket(`ws://127.0.0.1:${sdPort}`);

sdWs.on("open", () => {
    sdWs.send(JSON.stringify({ event: sdRegisterEvent, uuid: sdUUID }));
    console.log("Stream Deck connected");
});

sdWs.on("message", (data) => {
    const msg = JSON.parse(data.toString());

    if (msg.event === "keyDown") {
        const handler = actionHandlers[msg.action];
        if (handler && handler.keyDown) handler.keyDown();
    }
    else if (msg.event === "keyUp") {
        const handler = actionHandlers[msg.action];
        if (handler && handler.keyUp) handler.keyUp();
    }
    else if (msg.event === "willAppear") {
        const dial = dialFor(msg.action);
        if (dial) dial.willAppear(msg.context);
    }
    else if (msg.event === "willDisappear") {
        const dial = dialFor(msg.action);
        if (dial) dial.willDisappear(msg.context);
    }
    else if (msg.event === "dialRotate") {
        const dial = dialFor(msg.action);
        if (dial) dial.dialRotate(msg.context, (msg.payload && msg.payload.ticks) || 0);
    }
    else if (msg.event === "dialDown") {
        const dial = dialFor(msg.action);
        if (dial) dial.dialDown(msg.context);
    }
});

sdWs.on("close", () => { console.log("Stream Deck disconnected"); process.exit(0); });

// ── Start TCI connection ────────────────────────────────────────────────────
tciConnect();

console.log("AetherSDR Stream Deck plugin started");
