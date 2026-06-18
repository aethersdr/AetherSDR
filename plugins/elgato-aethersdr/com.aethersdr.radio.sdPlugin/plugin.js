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
const TCI_PORT = 40001;

function tciConnect() {
    try {
        tciWs = new WebSocket(`ws://${TCI_HOST}:${TCI_PORT}`);
        tciWs.on("open", () => { console.log("TCI connected"); });
        tciWs.on("message", (data) => parseTci(data.toString()));
        tciWs.on("close", () => { tciWs = null; tciScheduleReconnect(); });
        tciWs.on("error", () => {});
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
            case "vfo":          if (p.length >= 3) radio.frequency = parseInt(p[2]); break;
            case "modulation":   if (p.length >= 2) radio.mode = p[1]; break;
            case "trx":          if (p.length >= 2) radio.transmitting = p[1] === "true"; break;
            case "tune":         if (p.length >= 2) radio.tuning = p[1] === "true"; break;
            case "mute":         if (p.length >= 2) radio.muted = p[1] === "true"; break;
            case "volume":       if (p.length >= 1) radio.volume = parseInt(p[0]); break;
            case "drive":        if (p.length >= 1) radio.rfPower = parseInt(p[0]); break;
            case "tune_drive":   if (p.length >= 1) radio.tunePower = parseInt(p[0]); break;
            case "rx_nb":        if (p.length >= 2) radio.nbOn = p[1] === "true"; break;
            case "rx_nr":        if (p.length >= 2) radio.nrOn = p[1] === "true"; break;
            case "rx_anf":       if (p.length >= 2) radio.anfOn = p[1] === "true"; break;
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
    let best = 0, bestDist = Infinity;
    for (let i = 0; i < BAND_ORDER.length; i++) {
        const dist = Math.abs(freq - BANDS[BAND_ORDER[i]]);
        if (dist < bestDist) { bestDist = dist; best = i; }
    }
    return best;
}

// ── Action handlers ─────────────────────────────────────────────────────────
const POWER_LEVELS = [5, 10, 25, 50, 75, 100];
const TUNE_LEVELS = [5, 10, 15, 25, 50];
const TUNE_STEP_HZ = 1000; // 1 kHz per press; matches streamcontroller-aethersdr

function nextInCycle(levels, current) {
    return levels.find(l => l > current) || levels[0];
}

const actionHandlers = {
    // TX
    "com.aethersdr.radio.ptt":         { keyDown: () => tciSend("trx:0,true;"),  keyUp: () => tciSend("trx:0,false;") },
    "com.aethersdr.radio.mox-toggle":  { keyDown: () => tciSend(`trx:0,${!radio.transmitting};`) },
    "com.aethersdr.radio.tune-toggle": { keyDown: () => tciSend(`tune:0,${!radio.tuning};`) },
    "com.aethersdr.radio.rf-power":    { keyDown: () => tciSend(`drive:${nextInCycle(POWER_LEVELS, radio.rfPower)};`) },
    "com.aethersdr.radio.tune-power":  { keyDown: () => tciSend(`tune_drive:${nextInCycle(TUNE_LEVELS, radio.tunePower)};`) },
    // Audio
    "com.aethersdr.radio.mute-toggle":  { keyDown: () => tciSend(`mute:0,${!radio.muted};`) },
    "com.aethersdr.radio.volume-up":    { keyDown: () => tciSend(`volume:${Math.min(radio.volume + 5, 100)};`) },
    "com.aethersdr.radio.volume-down":  { keyDown: () => tciSend(`volume:${Math.max(radio.volume - 5, 0)};`) },
    // DSP
    "com.aethersdr.radio.nb-toggle":  { keyDown: () => tciSend(`rx_nb:0,${!radio.nbOn};`) },
    "com.aethersdr.radio.nr-toggle":  { keyDown: () => tciSend(`rx_nr:0,${!radio.nrOn};`) },
    "com.aethersdr.radio.anf-toggle": { keyDown: () => tciSend(`rx_anf:0,${!radio.anfOn};`) },
    "com.aethersdr.radio.apf-toggle": { keyDown: () => tciSend(`rx_apf:0,${!radio.apfOn};`) },
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
});

sdWs.on("close", () => { console.log("Stream Deck disconnected"); process.exit(0); });

// ── Start TCI connection ────────────────────────────────────────────────────
tciConnect();

console.log("AetherSDR Stream Deck plugin started");
