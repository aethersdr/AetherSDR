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
// TCI splits state into two scopes and the plugin has to mirror that split.
// Flattening them (as this file used to) means `rx_nb_enable:1,true;`
// overwrites the same field receiver 0 uses, so multi-slice state is actively
// wrong rather than merely absent.
//
//   per-slice  — every verb whose first argument is a <trx> ordinal
//   global     — verbs carrying no trx, plus the ones AetherSDR backs with
//                TransmitModel/master volume (one PA, one master fader) no
//                matter how the TCI spec words them

function newSliceState() {
    return {
        frequency: 0, rxVolume: 50,
        muted: false,
        nbOn: false, nrOn: false, anfOn: false, apfOn: false,
        sqlOn: false, split: false, locked: false,
        ritOn: false, xitOn: false,
    };
}

const slices = new Map(); // trx -> per-slice state

function slice(trx) {
    if (!slices.has(trx)) slices.set(trx, newSliceState());
    return slices.get(trx);
}

// Records arrive over a socket, so a truncated or malformed one must not be
// allowed to create state. `parseInt("")` is NaN and a Map keyed by NaN is a
// real entry, so an unvalidated trx would silently accumulate a phantom slice
// that no radio broadcast can ever correct. Returns null for anything that is
// not a non-negative integer.
function trxOf(text) {
    const n = parseInt(text, 10);
    return Number.isInteger(n) && n >= 0 ? n : null;
}

const radio = {
    transmitting: false, tuning: false,
    rfPower: 100, tunePower: 25,
    volume: 50, masterMuted: false, volumeBeforeMute: 50,
    // Slice tracking. txTrx follows tx_enable, activeTrx follows active_slice
    // (the GUI-focus broadcast added in #4160/#4323). `trx_count` is
    // deliberately NOT tracked — it is unreliable (see TARGET_MODES below) and
    // holding it would only invite building a slice picker on top of it.
    txTrx: 0, activeTrx: 0,
};

// ── Slice targeting ─────────────────────────────────────────────────────────
// Which slice the slice-scoped actions act on. "TRX0" stays the default so an
// existing layout behaves exactly as it did before this action existed.
//
// There is deliberately NO "target slice N" mode, and this is a design choice
// rather than an omission — TCI cannot support one safely:
//
//   - `trx_count` is sent ONCE, in the init burst (TciProtocol.cpp, its only
//     occurrence in the codebase). There are no slice add/remove events, so a
//     client that connects before the operator adds slices B and C is still
//     told there is one slice, forever. Observed directly: a session opened on
//     slice A alone reported `trx_count:1` and never revised it while two more
//     slices came up and broadcast state as trx 1 and trx 2.
//   - An out-of-range trx does not fail. TciProtocol::resolveSliceForTrx falls
//     through to `slices.first()`, so asking for a slice that no longer exists
//     silently acts on slice A instead. A picker built on a stale count would
//     therefore mis-target in complete silence, which for mode, lock or
//     squelch means changing the wrong receiver with no feedback at all.
//
// TX and ACTIVE avoid both problems: `tx_enable` and `active_slice` are
// broadcast on every change, so they are always current and always name a
// slice that exists. TRX0 is the one index that is safe to hardcode, since
// slice A is the one slice guaranteed to be present.
const TARGET_MODES = [
    { id: "trx0",   label: "Slice\nTRX0" },
    { id: "tx",     label: "Slice\nTX" },
    { id: "active", label: "Slice\nACTIVE" },
];
let targetModeIndex = 0;
let globalsSeeded = false;

// Which radio-authoritative values the radio has actually told us. A key
// painted with a plausible-looking default before the radio has said anything
// is worse than one painted "—": it reads as authoritative and it is wrong,
// which is what an operator would act on. Entries fill in from the init burst,
// from broadcasts, or from the GETs in requestState() below.
const known = new Set();

function shown(key, value) {
    return known.has(key) ? value : "—";
}

// The display guard above has a command-path twin. An action that computes an
// ABSOLUTE value from a relative gesture — step the frequency, cycle the power,
// nudge the volume — needs a baseline, and before the radio has reported there
// is none. Stepping from the struct's initial value means inventing one and
// sending it: from a cold start, Tune Down resolves to 30 kHz and Tune Power to
// 50 W. Refusing the gesture until the radio has spoken is the only answer
// consistent with Principle II; the value arrives within the init burst, so the
// refusal is never observable in normal use.
//
// Absolute actions — a band button, a mode, PTT — need no baseline and are not
// gated. Nor is anything the operator can still see the result of: the label
// shows "—" for exactly as long as the gesture is refused.
function needs(key) {
    return known.has(key);
}

function targetTrx() {
    switch (TARGET_MODES[targetModeIndex].id) {
        case "tx":     return radio.txTrx;
        case "active": return radio.activeTrx;
        default:       return 0;
    }
}

function targetSlice() {
    return slice(targetTrx());
}

// Everything that has to happen when the targeted slice changes, wherever the
// change came from.
//
// The operator-driven path (the Slice Target key) always did this. The
// radio-driven one — `active_slice` with Slice Target = ACTIVE, or `tx_enable`
// with TX — did not, and the two consequences were both dead ends rather than
// glitches:
//
//   * rx_volume is in neither the init burst nor any seed, so with nothing
//     re-requesting it, SLICE VOL+/- on the newly focused slice showed "—" and
//     stayed dead indefinitely.
//   * the dials kept rendering the previous slice's values — the VFO encoder
//     showing the old frequency and, when the new slice's vfo was unknown,
//     silently refusing rotation without ever painting the "—" that explains
//     why.
//
// Coalescing is requestState()'s job, so calling it per target move is cheap.
function retargetTo(previousTrx) {
    if (targetTrx() === previousTrx) return;
    bandCursor = null;          // the new target sits on its own band
    requestState();             // seed what this slice has never reported
    refreshKeypads();
    for (const d of DIALS) d.updateAll();
}

// ── TCI Client ──────────────────────────────────────────────────────────────
let tciWs = null;
let tciReconnectTimer = null;
// Overridable so test.js can drive this exact file against a fake server. The
// default is the only value an operator normally needs; without the override a
// test would have to bind the real port and would then be answered by — or
// collide with — a live AetherSDR or an already-running plugin instance.
const TCI_HOST = process.env.AETHERSDR_TCI_HOST || "localhost";
const TCI_PORT = Number(process.env.AETHERSDR_TCI_PORT) || 50001;

function tciConnect() {
    console.log(`Connecting to TCI at ws://${TCI_HOST}:${TCI_PORT}`);
    try {
        tciWs = new WebSocket(`ws://${TCI_HOST}:${TCI_PORT}`);
        tciWs.on("open", () => { console.log("TCI connected"); refreshKeypads(); requestState(); });
        tciWs.on("message", (data) => parseTci(data.toString()));
        tciWs.on("close", () => { tciWs = null; console.log("TCI disconnected, reconnecting in 3s"); refreshKeypads(); tciScheduleReconnect(); });
        tciWs.on("error", (e) => console.log("TCI error:", e && e.message));
    } catch (e) { tciScheduleReconnect(); }
}

function tciScheduleReconnect() {
    if (tciReconnectTimer) return;
    tciReconnectTimer = setTimeout(() => { tciReconnectTimer = null; tciConnect(); }, 3000);
}

function tciConnected() {
    return !!tciWs && tciWs.readyState === WebSocket.OPEN;
}

// Returns whether the command actually went out. Callers that flip local state
// optimistically MUST check it: a dropped send with the flag flipped anyway left
// the key lying about the radio — TUNE painting "TUNE ON" with no carrier, and
// needing two presses after reconnect to get back in step.
function tciSend(cmd) {
    if (!tciConnected()) return false;
    tciWs.send(cmd);
    return true;
}

// Ask the radio for everything the keys display, rather than trusting whatever
// happens to be left in memory. TciProtocol::handleCommand decides GET vs SET
// purely by argument count — `isSet = (args.size() >= 2)` — so every one-arg
// form below is a read and the replies arrive on the same path as broadcasts.
//
// `volume` is the exception and MUST stay bare: cmdVolume tests args.isEmpty(),
// but "volume:" splits to [""] which is NOT empty, so the colon form reaches
// the SET branch rather than the read. As of #4523 it fails that branch's
// argument check and is dropped — no reply, no notification — instead of
// being parsed as 0 dB and applied as 100%. So the colon form is still not a
// read: it is nothing at all. Sent without a colon the args list really is
// empty and it reads instead.
let stateRequestTimer = null;

function requestState() {
    if (stateRequestTimer || !tciConnected()) return;
    // Layout loads fire willAppear once per key; coalesce into one round.
    stateRequestTimer = setTimeout(() => {
        stateRequestTimer = null;
        if (!tciConnected()) return;
        const trx = targetTrx();
        tciSend(`drive:${radio.txTrx};`);
        tciSend(`tune_drive:${radio.txTrx};`);
        tciSend("volume;");
        tciSend(`vfo:${trx};`);
        tciSend(`rx_volume:${trx};`);
        for (const verb of Object.keys(SLICE_FLAGS)) {
            if (verb === "rx_mute") continue;  // same datum as `mute`
            tciSend(`${verb}:${trx};`);
        }
    }, 150);
}

// Slice-scoped boolean flags, keyed by TCI verb. Driving these from a table
// rather than a case each means the trx routing is written once and cannot
// drift between verbs as more are added.
const SLICE_FLAGS = {
    rx_nb_enable:  "nbOn",
    rx_nr_enable:  "nrOn",
    rx_anf_enable: "anfOn",
    rx_apf_enable: "apfOn",
    sql_enable:    "sqlOn",
    split_enable:  "split",
    lock:          "locked",
    rit_enable:    "ritOn",
    xit_enable:    "xitOn",
    // TciProtocol::cmdMute and cmdRxMute both write SliceModel::audioMute, so
    // the two verbs are the same datum under different names.
    mute:          "muted",
    rx_mute:       "muted",
};

function parseTci(msg) {
    // `;` is the TCI record terminator. AetherSDR happens to send exactly one
    // command per WebSocket frame (TciServer::sendInitBurst splits the burst
    // deliberately), so splitting on newlines alone worked by luck — but a
    // server that batches records into one frame would have every command
    // after the first silently swallowed. Split on the terminator itself and
    // both framings parse.
    for (const line of msg.split(/[;\n]/)) {
        const t = line.trim();
        if (!t) continue;
        const ci = t.indexOf(":");
        if (ci < 0) continue;
        const cmd = t.substring(0, ci).toLowerCase();
        const p = t.substring(ci + 1).split(",");

        const flagField = SLICE_FLAGS[cmd];
        if (flagField) {
            const trx = p.length >= 2 ? trxOf(p[0]) : null;
            if (trx !== null) {
                slice(trx)[flagField] = p[1] === "true";
                // Keyed on the field rather than the verb, so `mute` and
                // `rx_mute` — the same datum under two names — mark it once.
                known.add(`${flagField}:${trx}`);
            }
            continue;
        }

        switch (cmd) {
            // vfo:<trx>,<channel>,<hz> — channel 1 is the split/TX-B VFO, a
            // different slice's frequency, so only channel 0 updates this one.
            case "vfo": {
                const trx = trxOf(p[0]);
                const hz = parseInt(p[2], 10);
                if (p.length >= 3 && trx !== null && parseInt(p[1], 10) === 0
                    && Number.isFinite(hz) && hz > 0) {
                    // For the targeted slice the dial owns the value, so the
                    // echo has to go through its guard or the write below
                    // would clobber a spin in progress anyway.
                    known.add("vfo:" + trx);
                    // Inside the band-change window the FREQUENCY is guarded
                    // too, not just the cursor. Per the note above BANDS, the
                    // server keeps reporting the pre-change frequency for a
                    // moment after a band change — the guard stopped that from
                    // dragging bandCursor back, but nothing stopped it landing
                    // on slice.frequency, so a Tune Up pressed right after a
                    // Band Up stepped from the OLD band and pulled the radio
                    // back to it.
                    //
                    // Rather than ignore the whole window, take the echo that
                    // actually CONFIRMS the band we commanded, and let it end
                    // the window early. A stale echo names the old band and is
                    // dropped; the real one arrives and is applied at once, so
                    // this costs no responsiveness in the normal case.
                    let accept = true;
                    if (Date.now() < bandCursorGuardUntil) {
                        accept = bandCursor !== null && closestBandIndex(hz) === bandCursor;
                        if (accept) bandCursorGuardUntil = 0;   // confirmed
                    }
                    if (accept) {
                        if (trx === targetTrx()) vfoDial.acceptEcho(hz);
                        else slice(trx).frequency = hz;
                        if (Date.now() >= bandCursorGuardUntil) bandCursor = null;
                    }
                }
                break;
            }
            case "rx_volume": {
                const trx = trxOf(p[0]);
                const gain = parseInt(p[1], 10);
                if (p.length >= 2 && trx !== null && Number.isFinite(gain)) {
                    slice(trx).rxVolume = gain;
                    known.add("rx_volume:" + trx);
                }
                break;
            }
            // tx_enable:<trx>,<bool> — a TX move A->B arrives as TWO messages
            // (false for the old slice, then true for the new), so only the
            // true edge is authoritative.
            case "tx_enable": {
                const trx = trxOf(p[0]);
                if (p.length >= 2 && p[1] === "true" && trx !== null) {
                    const before = targetTrx();
                    radio.txTrx = trx;
                    retargetTo(before);
                }
                break;
            }
            case "active_slice": {
                const trx = trxOf(p[0]);
                if (trx !== null) {
                    const before = targetTrx();
                    radio.activeTrx = trx;
                    retargetTo(before);
                }
                break;
            }
            // One PA, so transmit state is radio-global regardless of the trx
            // the server labels it with.
            case "trx":
                if (p.length >= 2) {
                    radio.transmitting = p[1] === "true";
                    // TUNE has no broadcast of its own (TciProtocol::cmdTune
                    // SET returns nothing and TciServer never emits `tune:`),
                    // but a tune carrier keys the radio — so trx going false
                    // proves the carrier stopped, including when the operator
                    // stopped it from the AetherSDR GUI. Without this the
                    // optimistic tune flag desyncs the moment TUNE is stopped
                    // anywhere but here.
                    if (!radio.transmitting) radio.tuning = false;
                }
                break;
            case "tune":        if (p.length >= 2) radio.tuning = p[1] === "true"; break;
            case "volume": {
                const db = parseInt(p[0], 10);
                if (p.length >= 1 && Number.isFinite(db)) {
                    // Do NOT write radio.volume here. It is this dial's
                    // getValue/setValue, so assigning it directly applies the
                    // echo before acceptEcho() can decline it — which made the
                    // mid-spin guard inert and let the value snap back, the
                    // exact stall the guard exists to stop. Only the dial may
                    // write the field it owns. (Same for drive/tune_drive
                    // below; `vfo` already routed correctly, which is why the
                    // bug survived: the one dial the tests covered was fine.)
                    const pct = volumePercentFromDb(db);
                    known.add("volume");
                    // Mute All is a plugin-side notion — TCI has no master-mute
                    // verb, so it drives master volume to the silence floor and
                    // restores the previous level. If audio comes back by any
                    // other route (the AetherSDR fader, another client), the
                    // operator is no longer muted and the remembered level is
                    // stale. Clearing the flag here stops a later unmute from
                    // writing that stale value back over what the radio now
                    // reports.
                    // Judged on the RADIO's reported level, not the dial's:
                    // this is about whether audio actually came back, which is
                    // true regardless of whether a spin is in progress.
                    if (radio.masterMuted && pct > 0) radio.masterMuted = false;
                    volumeDial.acceptEcho(pct);
                }
                break;
            }
            // TCI v2 DRIVE/TUNE_DRIVE replies are always `<trx>,<power>`.
            // Keep last-field parsing for compatibility with older servers.
            case "drive": {
                const w = parseInt(p[p.length - 1], 10);
                if (Number.isFinite(w)) {
                    known.add("drive");
                    // The dial owns radio.rfPower — see the note under `volume`.
                    rfPowerDial.acceptEcho(w);
                }
                break;
            }
            case "tune_drive": {
                const w = parseInt(p[p.length - 1], 10);
                if (Number.isFinite(w)) {
                    known.add("tune_drive");
                    // The dial owns radio.tunePower — see the note under `volume`.
                    tunePowerDial.acceptEcho(w);
                }
                break;
            }
        }
    }
    refreshKeypads();
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

// Band up/down cursor. Deriving the current band from the reported VFO on
// every press looks simpler but breaks against the server as it stands: a
// band change moves the panadapter (`dds:` follows, and so do modulation and
// rx_filter_band) while `vfo:` keeps reporting the pre-change frequency. Band
// stepping then recomputes from that stale value every press and stays parked
// one band away from where the operator actually is.
//
// The cursor holds what WE last commanded instead. It resyncs from the
// reported frequency only outside the guard window below, so an operator
// tuning elsewhere is still picked up, while the stale post-band-change echo
// cannot drag stepping back. The server-side behaviour is a separate defect
// and is not worked around any further than this.
const BAND_CURSOR_GUARD_MS = 2000;
let bandCursor = null;
let bandCursorGuardUntil = 0;

function bandIndexForStep() {
    if (bandCursor === null) bandCursor = closestBandIndex(targetSlice().frequency);
    return bandCursor;
}

function tuneToBand(index) {
    bandCursor = index;
    bandCursorGuardUntil = Date.now() + BAND_CURSOR_GUARD_MS;
    const trx = targetTrx();
    const hz = BANDS[BAND_ORDER[index]];
    slice(trx).frequency = hz;  // optimistic; see note above
    tciSend(`vfo:${trx},0,${hz};`);
}

// ── Action handlers ─────────────────────────────────────────────────────────
const POWER_LEVELS = [5, 10, 25, 50, 75, 100];
const TUNE_LEVELS = [5, 10, 15, 25, 50];
const TUNE_STEP_HZ = 1000; // 1 kHz per press; matches streamcontroller-aethersdr
const RX_VOLUME_STEP = 10; // SliceModel::audioGain is 0-100 linear

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

// ── Stream Deck send helpers ─────────────────────────────────────────────────
function sdSend(payload) {
    if (sdWs && sdWs.readyState === WebSocket.OPEN) sdWs.send(JSON.stringify(payload));
}

// ── Keypad context tracking ──────────────────────────────────────────────────
// There was no keypad registry at all before this: willAppear forwarded to
// dials and discarded msg.context for everything else, so no key could ever
// have its title or state updated and every label stayed frozen at whatever
// manifest.json declared. Mirrors createDial's `instances` Map.
const keypads = new Map(); // context -> action UUID

// Title providers. An action absent from this table keeps its manifest title,
// so only the keys whose label carries live state pay for the redraw.
const keypadTitles = {
    "com.aethersdr.radio.rf-power":     () => `RF PWR\n${shown("drive", radio.rfPower + " W")}`,
    "com.aethersdr.radio.tune-power":   () => `TUNE PWR\n${shown("tune_drive", radio.tunePower + " W")}`,
    "com.aethersdr.radio.tune-toggle":  () => (radio.tuning ? "TUNE\nON" : "TUNE"),
    "com.aethersdr.radio.slice-target": () => TARGET_MODES[targetModeIndex].label,
    "com.aethersdr.radio.mute-all":     () => (radio.masterMuted ? "UNMUTE\nALL" : "MUTE\nALL"),
    "com.aethersdr.radio.tci-status":   () => (tciConnected() ? "TCI\nOK" : "TCI\nDOWN"),
    "com.aethersdr.radio.slice-volume-up":   () => `SLICE VOL+\n${shown("rx_volume:" + targetTrx(), targetSlice().rxVolume)}`,
    "com.aethersdr.radio.slice-volume-down": () => `SLICE VOL-\n${shown("rx_volume:" + targetTrx(), targetSlice().rxVolume)}`,
};

// Last title actually pushed per context, so an unchanged title costs nothing.
// This matters more than it looks: TciServer::broadcastStatus emits rx_smeter
// per slice every 200 ms to every client, so parseTci runs ~15 times a second
// with three slices. Repainting every key on each of those would put a
// continuous stream of setTitle at the host — and a redraw at the device — for
// values that never changed.
const lastTitle = new Map();

// `force` re-sends even an unchanged title: the redraw nudge below has to
// overwrite a label the host painted behind our back, which a "nothing
// changed" check would otherwise skip.
function refreshKeypads(force = false) {
    for (const [context, action] of keypads) {
        const title = keypadTitles[action];
        if (!title) continue;
        const text = title();
        if (!force && lastTitle.get(context) === text) continue;
        lastTitle.set(context, text);
        sdSend({ event: "setTitle", context, payload: { title: text, target: 0 } });
    }
}

// A newly created action is painted from the Title its manifest state declares,
// and that paint can land AFTER the willAppear we answer — so one setTitle at
// willAppear is not reliably the last word. Re-assert over the first couple of
// seconds so ours wins regardless of ordering. Forced, because the host may
// have overwritten the key with its manifest label since our last send while
// our cached value still matches.
// Coalesced for the same reason as nudgeRedrawAll: refreshKeypads() already
// walks every key, so one pending set of timers per layout event does the whole
// deck. Calling it once per appearing key made a profile load quadratic in the
// forced repaints — ~2.9k setTitle for 50 keys, against ~200 coalesced.
let soonTimers = null;

function refreshKeypadsSoon() {
    const force = () => refreshKeypads(true);
    force();
    if (soonTimers) return;
    soonTimers = [
        setTimeout(force, 250),
        setTimeout(force, 1000),
        setTimeout(() => { soonTimers = null; force(); }, 2500),
    ];
}

// A key added mid-session renders its manifest label — on the device as well
// as in the editor — until the host is restarted. The title data is fine:
// OpenDeck stores our text against the key instance and renders it correctly
// after a restart, so what is missing is a redraw trigger.
//
// setImage with no `image` in the payload resets the key to its manifest
// image, i.e. the picture it is already showing, so it is a visual no-op that
// still pushes the button through the render path and composites the stored
// title.
//
// It has to reach EVERY key, not just the new one: adding an action re-renders
// the whole deck from the action definitions, so nudging only the newcomer
// left it correct while knocking every other key back to its manifest label.
// Titles are re-sent first so the render has the right text to composite, and
// the pair is repeated once in case the host's own render lands in between.
// Coalesced to one round per layout event, not one per key. `push()` already
// walks every registered keypad, so calling this once per appearing key made a
// profile load quadratic — N keys produced N x 2 x N setImage messages, ~14.5k
// host messages for a 50-key layout. A single pending handle (the same shape
// requestState() uses for its GET round) collapses that to two pushes however
// many keys arrive.
let nudgeTimers = null;

function nudgeRedrawAll() {
    if (nudgeTimers) return;
    const push = () => {
        refreshKeypads(true);
        for (const context of keypads.keys()) {
            sdSend({ event: "setImage", context, payload: { target: 0 } });
        }
    };
    nudgeTimers = [
        setTimeout(push, 120),
        setTimeout(() => { nudgeTimers = null; push(); }, 600),
    ];
}

// Tied to layout events only — never a periodic timer. The title itself is
// never lost: OpenDeck applies and persists ours against the key instance (the
// stored profile holds it) and renders it correctly after a restart. Only the
// redraw trigger is missing, and that is needed just when the deck re-renders,
// so a repeating repaint would be pure churn.

// ── Dial (Encoder) support ───────────────────────────────────────────────────
// Shared by every Stream Deck+ knob: each rotate/press is driven by caller-
// supplied nextValue()/onPress() rules, and mirrored back via setFeedback.
// Sending one TCI command per tick would flood the socket during a fast
// spin, so the outgoing command is debounced to one per quiet window while
// the local value (and on-dial feedback) updates immediately.
const DIAL_SEND_DEBOUNCE_MS = 25;

// How long after a rotate to ignore inbound echoes for that dial. AetherSDR
// re-broadcasts a changed value from several places at once (the model-change
// relay in TciServer::broadcastSliceFrequencies plus the completion
// confirmation in tuneSliceAndConfirm), and every one of them lands mid-spin
// and overwrites the ticks accumulated since the last send — so the dial
// fights the radio and stalls on one value. This is the same echo-bounce
// class as the RF power slider in #4350; the guard is the dial's equivalent
// of the drag hold that fixed it.
const DIAL_ECHO_GUARD_MS = 400;

// `ready` is the dial's half of the command guard: a rotate is a relative
// gesture, so nextValue() needs a baseline the radio supplied. Without it the
// first detent resolves against the struct's initial value — 0 Hz clamps to the
// 30 kHz floor and sends the radio there. Refused until the value is known; the
// dial reads "—" for exactly that long.
function createDial({ uuid, initialState, nextValue, onPress, getValue, setValue, sendCommand, formatTitle, formatValue, ready }) {
    const instances = new Map(); // context -> per-instance mutable state (e.g. stepIndex)
    let sendTimer = null;
    let guardUntil = 0;

    function sendFeedback(context) {
        const state = instances.get(context);
        if (!state) return;
        sdSend({
            event: "setFeedback",
            context,
            payload: { title: formatTitle(state), value: formatValue(getValue()) },
        });
    }

    return {
        uuid,
        updateAll() { for (const context of instances.keys()) sendFeedback(context); },
        // Apply a value the radio reported, unless this dial is mid-spin.
        acceptEcho(v) {
            if (Date.now() < guardUntil) return;
            setValue(v);
            this.updateAll();
        },
        willAppear(context) { instances.set(context, initialState()); sendFeedback(context); },
        willDisappear(context) { instances.delete(context); },
        dialRotate(context, ticks) {
            const state = instances.get(context);
            if (!state) return;
            if (ready && !ready()) return;
            guardUntil = Date.now() + DIAL_ECHO_GUARD_MS;
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
const TUNE_POWER_DIAL_UUID = "com.aethersdr.radio.tune-power-dial";
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
    getValue: () => targetSlice().frequency,
    setValue: (v) => { targetSlice().frequency = v; },
    sendCommand: (hz) => tciSend(`vfo:${targetTrx()},0,${hz};`),
    ready: () => needs(`vfo:${targetTrx()}`),
    formatTitle: (state) => `Step: ${formatStepHz(VFO_STEPS[state.stepIndex])}`,
    formatValue: (hz) => shown("vfo:" + targetTrx(), formatFreqMHz(hz)),
});

const RF_POWER_STEPS = [1, 5];

const rfPowerDial = createDial({
    uuid: RF_POWER_DIAL_UUID,
    initialState: () => ({ stepIndex: 0 }),
    nextValue: (cur, ticks, state) => clamp(cur + ticks * RF_POWER_STEPS[state.stepIndex], 0, 100),
    onPress: (state) => { state.stepIndex = (state.stepIndex + 1) % RF_POWER_STEPS.length; },
    getValue: () => radio.rfPower,
    setValue: (v) => { radio.rfPower = v; },
    sendCommand: (w) => tciSend(`drive:${radio.txTrx},${w};`),
    ready: () => needs("drive"),
    formatTitle: (state) => `Step: ${RF_POWER_STEPS[state.stepIndex]} W`,
    formatValue: (w) => shown("drive", `${w} W`),
});

// Tune power lives on its own dial rather than sharing the RF power one: they
// are separate radio settings (TransmitModel rfPower vs tunePower) and an
// operator setting a low tune level wants it to stay put while RF power moves.
// Same 0-100 range and step cycle as RF power so the two feel identical.
const tunePowerDial = createDial({
    uuid: TUNE_POWER_DIAL_UUID,
    initialState: () => ({ stepIndex: 0 }),
    nextValue: (cur, ticks, state) => clamp(cur + ticks * RF_POWER_STEPS[state.stepIndex], 0, 100),
    onPress: (state) => { state.stepIndex = (state.stepIndex + 1) % RF_POWER_STEPS.length; },
    getValue: () => radio.tunePower,
    setValue: (v) => { radio.tunePower = v; },
    sendCommand: (w) => tciSend(`tune_drive:${radio.txTrx},${w};`),
    ready: () => needs("tune_drive"),
    formatTitle: (state) => `Step: ${RF_POWER_STEPS[state.stepIndex]} W`,
    formatValue: (w) => shown("tune_drive", `${w} W`),
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
    // "mute" is per-slice (TciProtocol::cmdMute writes SliceModel::audioMute),
    // so pressing the master volume dial mutes the targeted slice. Its echo
    // does arrive via the #4430 flag relay, but a press must still feel
    // instant, so the local flag leads and the broadcast confirms it.
    onPress: () => {
        const trx = targetTrx();
        if (!needs(`muted:${trx}`)) return;
        const s = slice(trx);
        const next = !s.muted;
        if (tciSend(`mute:${trx},${next};`)) s.muted = next;
    },
    getValue: () => radio.volume,
    setValue: (v) => { radio.volume = v; },
    sendCommand: (v) => tciSend(volumeSetCommand(v)),
    ready: () => needs("volume"),
    formatTitle: () => "Push: Mute Slice",
    formatValue: (v) => shown("volume", `${v}%`),
});

const DIALS = [vfoDial, rfPowerDial, tunePowerDial, volumeDial];
function dialFor(action) { return DIALS.find(d => d.uuid === action); }

// Slice-scoped sends resolve their trx through targetTrx(). PTT and MOX
// deliberately do NOT — but not because the trx picks the slice. It doesn't:
// the server discards it. `trx:` is recorded as a TrxRequest and resolved by
// TciRoutingState::resolvePttSlice, which returns the *live* TX slice whenever
// the requested one differs from it, and otherwise falls through to a
// server-wide cached route (`m_txSliceId`, torn down only when the last TCI
// client disconnects).
//
// That first branch is also the only thing that refreshes the cache. Sending
// the TX trx makes requested == live on every press, so it never fires and a
// route left stale by another client — WSJT-X negotiating split, then the
// operator moving TX in the GUI — decides where we key. Sending 0 keeps the
// refresh alive for every configuration except TX-on-the-first-slice, so it is
// the narrower hole, not a safe one. See #4547; revisit when that lands.
const actionHandlers = {
    // TX
    "com.aethersdr.radio.ptt":         { keyDown: () => tciSend("trx:0,true;"), keyUp: () => tciSend("trx:0,false;") },
    "com.aethersdr.radio.mox-toggle":  { keyDown: () => tciSend(`trx:0,${!radio.transmitting};`) },
    // TUNE takes a trx but TciProtocol::cmdTune discards it (`(void)trx;`), so
    // the value is cosmetic — 0 to match the pair above.
    //
    // cmdTune SET produces no notification and TciServer emits no `tune:`
    // broadcast at all, so radio.tuning could never leave its initial false
    // and every press re-sent `tune:...,true` — TUNE would start but never
    // stop. Lead with the local flag; the trx: handler above resyncs it.
    // Commit the optimistic flip only if the command actually went out — see
    // tciSend(). With TCI down this used to paint "TUNE ON" over a radio that
    // was never told, and took two presses after reconnect to resync.
    "com.aethersdr.radio.tune-toggle": { keyDown: () => { const next = !radio.tuning; if (tciSend(`tune:0,${next};`)) radio.tuning = next; refreshKeypads(); } },
    // drive/tune_drive are radio-global (one PA) and AetherSDR backs them with
    // TransmitModel, so they address the TX slice rather than the target.
    // Cycling steps from the current level, so both need the radio's value
    // first. `cmdDrive`/`cmdTuneDrive` SET is not gated on a connected radio
    // server-side — it writes TransmitModel whenever the model exists — so the
    // plugin is the only thing standing in front of a fabricated power level.
    "com.aethersdr.radio.rf-power":    { keyDown: () => { if (!needs("drive")) return; radio.rfPower = nextInCycle(POWER_LEVELS, radio.rfPower); tciSend(`drive:${radio.txTrx},${radio.rfPower};`); rfPowerDial.updateAll(); refreshKeypads(); } },
    "com.aethersdr.radio.tune-power":  { keyDown: () => { if (!needs("tune_drive")) return; radio.tunePower = nextInCycle(TUNE_LEVELS, radio.tunePower); tciSend(`tune_drive:${radio.txTrx},${radio.tunePower};`); tunePowerDial.updateAll(); refreshKeypads(); } },
    // Audio — master fader is radio-global, slice volume/mute are per-slice.
    "com.aethersdr.radio.mute-toggle":  { keyDown: () => { const trx = targetTrx(); if (!needs(`muted:${trx}`)) return; const s = slice(trx); const next = !s.muted; if (tciSend(`mute:${trx},${next};`)) s.muted = next; } },
    "com.aethersdr.radio.volume-up":    { keyDown: () => { if (!needs("volume")) return; tciSend(volumeSetCommand(VOLUME_LEVELS[Math.min(closestIndex(VOLUME_LEVELS, radio.volume) + 1, VOLUME_LEVELS.length - 1)])); } },
    "com.aethersdr.radio.volume-down":  { keyDown: () => { if (!needs("volume")) return; tciSend(volumeSetCommand(VOLUME_LEVELS[Math.max(closestIndex(VOLUME_LEVELS, radio.volume) - 1, 0)])); } },
    // There is no global-mute verb in TCI — `mute` and `rx_mute` both write
    // SliceModel::audioMute for one slice. Silencing everything therefore
    // means driving the master fader to the spec's silence floor and
    // restoring the previous level on the way back.
    "com.aethersdr.radio.mute-all": {
        keyDown: () => {
            // Gated like every other volume action. Without this, a mute
            // followed by an unmute before the init burst arrived commanded
            // master volume to radio.volume's FABRICATED default of 50% — the
            // one thing the `needs()` gate exists to prevent, and this was the
            // only volume key not behind it.
            if (!needs("volume")) return;
            // Commit only on a successful send, like the other toggles: a key
            // reading UNMUTE ALL over a radio still at full volume is worse
            // than one that did not appear to respond.
            if (!radio.masterMuted) {
                const before = radio.volume;
                if (tciSend(volumeSetCommand(0))) {
                    radio.volumeBeforeMute = before;
                    radio.masterMuted = true;
                }
            } else {
                // No `|| 50` fallback: 0 is a legitimate level, and the field
                // is seeded at 50 and only ever assigned from radio.volume, so
                // a falsy-coalesce could not protect against undefined — it
                // could only turn a genuine silence into a jump to half volume
                // on a key labelled UNMUTE.
                if (tciSend(volumeSetCommand(radio.volumeBeforeMute))) radio.masterMuted = false;
            }
            refreshKeypads();
        },
    },
    "com.aethersdr.radio.slice-volume-up":   { keyDown: () => { const trx = targetTrx(); if (!needs(`rx_volume:${trx}`)) return; const s = slice(trx); s.rxVolume = clamp(s.rxVolume + RX_VOLUME_STEP, 0, 100); tciSend(`rx_volume:${trx},${s.rxVolume};`); refreshKeypads(); } },
    "com.aethersdr.radio.slice-volume-down": { keyDown: () => { const trx = targetTrx(); if (!needs(`rx_volume:${trx}`)) return; const s = slice(trx); s.rxVolume = clamp(s.rxVolume - RX_VOLUME_STEP, 0, 100); tciSend(`rx_volume:${trx},${s.rxVolume};`); refreshKeypads(); } },
    // DSP — all per-slice.
    "com.aethersdr.radio.nb-toggle":  { keyDown: () => { const trx = targetTrx(); if (!needs(`nbOn:${trx}`)) return; const s = slice(trx); s.nbOn = !s.nbOn; tciSend(`rx_nb_enable:${trx},${s.nbOn};`); } },
    "com.aethersdr.radio.nr-toggle":  { keyDown: () => { const trx = targetTrx(); if (!needs(`nrOn:${trx}`)) return; const s = slice(trx); s.nrOn = !s.nrOn; tciSend(`rx_nr_enable:${trx},${s.nrOn};`); } },
    "com.aethersdr.radio.anf-toggle": { keyDown: () => { const trx = targetTrx(); if (!needs(`anfOn:${trx}`)) return; const s = slice(trx); s.anfOn = !s.anfOn; tciSend(`rx_anf_enable:${trx},${s.anfOn};`); } },
    "com.aethersdr.radio.apf-toggle": { keyDown: () => { const trx = targetTrx(); if (!needs(`apfOn:${trx}`)) return; const s = slice(trx); s.apfOn = !s.apfOn; tciSend(`rx_apf_enable:${trx},${s.apfOn};`); } },
    "com.aethersdr.radio.sql-toggle": { keyDown: () => { const trx = targetTrx(); if (!needs(`sqlOn:${trx}`)) return; const s = slice(trx); s.sqlOn = !s.sqlOn; tciSend(`sql_enable:${trx},${s.sqlOn};`); } },
    // Slice
    "com.aethersdr.radio.split-toggle": { keyDown: () => { const trx = targetTrx(); if (!needs(`split:${trx}`)) return; tciSend(`split_enable:${trx},${!slice(trx).split};`); } },
    "com.aethersdr.radio.lock-toggle":  { keyDown: () => { const trx = targetTrx(); if (!needs(`locked:${trx}`)) return; tciSend(`lock:${trx},${!slice(trx).locked};`); } },
    "com.aethersdr.radio.rit-toggle":   { keyDown: () => { const trx = targetTrx(); if (!needs(`ritOn:${trx}`)) return; tciSend(`rit_enable:${trx},${!slice(trx).ritOn};`); } },
    "com.aethersdr.radio.xit-toggle":   { keyDown: () => { const trx = targetTrx(); if (!needs(`xitOn:${trx}`)) return; tciSend(`xit_enable:${trx},${!slice(trx).xitOn};`); } },
    // Target selector
    "com.aethersdr.radio.slice-target": {
        keyDown: () => {
            const before = targetTrx();
            targetModeIndex = (targetModeIndex + 1) % TARGET_MODES.length;
            sdSend({ event: "setGlobalSettings", context: sdUUID, payload: { targetMode: TARGET_MODES[targetModeIndex].id } });
            // Same work as a radio-driven move, via the same helper, so the two
            // paths cannot drift apart again. Cycling back to the same trx (one
            // slice open, so TX and ACTIVE resolve alike) legitimately does
            // nothing but relabel the key, which refreshKeypads() below covers.
            retargetTo(before);
            refreshKeypads();
        },
    },
    "com.aethersdr.radio.tci-status": { keyDown: () => refreshKeypads() },
    // Frequency. Stepping needs a baseline; a direct band button does not.
    "com.aethersdr.radio.tune-up":   { keyDown: () => { const trx = targetTrx(); if (!needs(`vfo:${trx}`)) return; const s = slice(trx); s.frequency += TUNE_STEP_HZ; tciSend(`vfo:${trx},0,${s.frequency};`); } },
    "com.aethersdr.radio.tune-down": { keyDown: () => { const trx = targetTrx(); if (!needs(`vfo:${trx}`)) return; const s = slice(trx); s.frequency = Math.max(VFO_FREQ_MIN, s.frequency - TUNE_STEP_HZ); tciSend(`vfo:${trx},0,${s.frequency};`); } },
    "com.aethersdr.radio.band-up":   { keyDown: () => { if (!needs(`vfo:${targetTrx()}`)) return; tuneToBand(Math.min(bandIndexForStep() + 1, BAND_ORDER.length - 1)); } },
    "com.aethersdr.radio.band-down": { keyDown: () => { if (!needs(`vfo:${targetTrx()}`)) return; tuneToBand(Math.max(bandIndexForStep() - 1, 0)); } },
    // DVK
    "com.aethersdr.radio.dvk-play":   { keyDown: () => tciSend(`rx_play:${targetTrx()},true;`) },
    "com.aethersdr.radio.dvk-record": { keyDown: () => tciSend(`rx_record:${targetTrx()},true;`) },
};

// Band actions
for (const band of BAND_ORDER) {
    actionHandlers[`com.aethersdr.radio.band-${band}`] = { keyDown: () => tuneToBand(BAND_ORDER.indexOf(band)) };
}

// Mode actions. Values must be names TciProtocol::tciToSmartSDR knows —
// as of #4523, anything else is rejected by the server (dropped, no reply,
// no notification) rather than silently substituted with USB, so a typo
// here now does nothing instead of keying the wrong mode.
// There is no FT8 entry: FT8 is not a modulation, it is an operating
// convention on DIGU. Before #4523 the old `mode-ft8` action's "ft8" wasn't
// in the map and silently keyed SSB via the USB fallback; mapping it to DIGU
// fixed that but made it a byte-for-byte duplicate of the DIGU action
// (verified on air: both emit `modulation:<trx>,digu` and the same
// `rx_filter_band 0,3000`), so the action was dropped rather than shipped
// twice under two names.
const MODES = {
    usb: "usb",   lsb: "lsb",   cw: "cw",     cwr: "cwr",
    am: "am",     sam: "sam",   fm: "fm",     nfm: "nfm",
    digu: "digu", digl: "digl", rtty: "rtty",
};
for (const [action, tciMode] of Object.entries(MODES)) {
    actionHandlers[`com.aethersdr.radio.mode-${action}`] = { keyDown: () => tciSend(`modulation:${targetTrx()},${tciMode};`) };
}

// ── Stream Deck WebSocket connection ────────────────────────────────────────
const sdWs = new WebSocket(`ws://127.0.0.1:${sdPort}`);

sdWs.on("open", () => {
    sdWs.send(JSON.stringify({ event: sdRegisterEvent, uuid: sdUUID }));
    // Global settings are plugin-side data — readable and writable from the
    // plugin process with no Property Inspector involved, which is what makes
    // the slice-target selection persist across restarts.
    sdSend({ event: "getGlobalSettings", context: sdUUID });
    console.log("Stream Deck connected");
});

// Diagnostic: which events this host actually sends, and for which action.
// Only the first of each kind is logged so the log stays readable.
const seenEvents = new Set();

sdWs.on("message", (data) => {
    const msg = JSON.parse(data.toString());

    if (msg.event && !seenEvents.has(msg.event)) {
        seenEvents.add(msg.event);
        console.log(`first ${msg.event}${msg.action ? " for " + msg.action.split(".").pop() : ""}`);
    }

    // Learn a key's identity from ANY event that names both an action and a
    // context — not just willAppear. A host that does not announce an action
    // the moment it is dropped leaves the plugin with no context to paint, so
    // the key keeps its manifest label until the host restarts. Every other
    // event that mentions the action (title changes, settings, a press) is an
    // equally good introduction, and taking whichever arrives first removes
    // the dependency on one specific event ever being sent.
    if (msg.context && msg.action && typeof msg.context === "string"
        && !dialFor(msg.action) && !keypads.has(msg.context)
        && msg.event !== "willDisappear") {
        keypads.set(msg.context, msg.action);
        console.log(`learned ${msg.action.split(".").pop()} via ${msg.event}`);
        refreshKeypadsSoon();
        requestState();
        // No nudge here: the willAppear branch below covers the layout event,
        // and nudging from both meant two calls for the same message.
    }

    if (msg.event === "keyDown") {
        const handler = actionHandlers[msg.action];
        if (handler && handler.keyDown) handler.keyDown();
    }
    else if (msg.event === "keyUp") {
        const handler = actionHandlers[msg.action];
        if (handler && handler.keyUp) handler.keyUp();
    }
    // Both of these accompany a deck-wide re-render that knocks every key back
    // to its manifest label, so each has to repaint and nudge ALL keys — not
    // just the one the event names. Keypad registration itself is handled by
    // the generic learner above.
    else if (msg.event === "willAppear") {
        const dial = dialFor(msg.action);
        if (dial) dial.willAppear(msg.context);
        else { refreshKeypadsSoon(); nudgeRedrawAll(); }
    }
    else if (msg.event === "willDisappear") {
        const dial = dialFor(msg.action);
        if (dial) dial.willDisappear(msg.context);
        else { keypads.delete(msg.context); refreshKeypadsSoon(); nudgeRedrawAll(); }
    }
    else if (msg.event === "dialRotate") {
        const dial = dialFor(msg.action);
        if (dial) dial.dialRotate(msg.context, (msg.payload && msg.payload.ticks) || 0);
    }
    else if (msg.event === "dialDown") {
        const dial = dialFor(msg.action);
        if (dial) dial.dialDown(msg.context);
    }
    else if (msg.event === "didReceiveGlobalSettings") {
        // Seed ONCE, at startup. Stream Deck re-sends this event whenever
        // global settings change and on layout events such as deleting an
        // action — so applying it every time let a stored value overwrite the
        // operator's live in-session choice. The selection then disagreed with
        // the key face: the radio was still being driven from the real value
        // while the label showed the restored one.
        if (!globalsSeeded) {
            globalsSeeded = true;
            const saved = msg.payload && msg.payload.settings && msg.payload.settings.targetMode;
            const i = TARGET_MODES.findIndex(m => m.id === saved);
            if (i >= 0) targetModeIndex = i;
        }
        refreshKeypads();
    }
});

sdWs.on("close", () => { console.log("Stream Deck disconnected"); process.exit(0); });

// ── Start TCI connection ────────────────────────────────────────────────────
tciConnect();

console.log("AetherSDR Stream Deck plugin started");
