"use strict";
/**
 * Tests for the AetherSDR Elgato Stream Deck plugin.
 *
 * Run with `npm test`, or `node --test test.js`.
 *
 * Uses only node:test and node:assert, so the plugin keeps the "no build step,
 * no npm dependencies" property stated at the top of plugin.js — `ws` is the
 * single runtime dependency and the plugin already needs it.
 *
 * Every behavioural test spawns the real plugin.js as a child process against a
 * fake Stream Deck socket and a fake TCI server, so what is exercised is the
 * file that ships rather than a refactored copy. Both servers bind port 0 and
 * the TCI port is handed to the child through AETHERSDR_TCI_PORT, so a test run
 * cannot collide with a live AetherSDR or an already-running plugin instance.
 */

const { test } = require("node:test");
const assert = require("node:assert");
const { once } = require("node:events");
const { spawn } = require("node:child_process");
const fs = require("node:fs");
const path = require("node:path");
const { WebSocketServer } = require("ws");

const DIR = __dirname;
const manifest = JSON.parse(fs.readFileSync(path.join(DIR, "manifest.json"), "utf8"));
const sleep = (ms) => new Promise((r) => setTimeout(r, ms));

// ── Harness ─────────────────────────────────────────────────────────────────

// Radio state the fake server reports. Deliberately unlike the plugin's own
// initial values, so a test cannot pass by reading an uninitialised default.
const RADIO = { drive: 37, tuneDrive: 7, volumeDb: -6, rxVolume: 42, vfoHz: 14074000 };

/**
 * Boots the plugin against fake sockets, runs `body`, then tears everything
 * down. `body` receives helpers plus the recorded traffic in both directions.
 */
async function withPlugin(body, { silent = false } = {}) {
    const sd = new WebSocketServer({ port: 0, host: "127.0.0.1" });
    const tci = new WebSocketServer({ port: 0, host: "127.0.0.1" });
    await Promise.all([once(sd, "listening"), once(tci, "listening")]);

    const sent = [];        // TCI commands the plugin sent
    const titles = new Map();   // context -> latest title
    const feedback = new Map(); // context -> latest dial feedback
    let images = 0;             // setImage count
    let titleWrites = 0;        // setTitle count — distinct from titles.size,
                                // because a repaint storm re-sends unchanged text
    let radioWs = null;

    tci.on("connection", (ws) => {
        radioWs = ws;
        ws.on("message", (d) => {
            const text = d.toString().trim();
            sent.push(text);
            if (silent) return;          // accept the socket, answer nothing
            for (const record of text.split(";").filter(Boolean)) {
                answerGet(ws, record);
            }
        });
        // Init burst, one command per frame as TciServer::sendInitBurst does.
        if (silent) return;
        for (const c of [
            "trx_count:1;", "tx_enable:0,true;",
            `vfo:0,0,${RADIO.vfoHz};`, `drive:0,${RADIO.drive};`,
            `tune_drive:0,${RADIO.tuneDrive};`, `volume:${RADIO.volumeDb};`,
            `rx_volume:0,${RADIO.rxVolume};`, "active_slice:0,A;",
            ...FLAG_VERBS.map((v) => `${v}:0,false;`),
            "ready;", "start;",
        ]) ws.send(c);
    });

    const child = spawn(process.execPath, [
        "plugin.js",
        "-port", String(sd.address().port),
        "-pluginUUID", "TEST",
        "-registerEvent", "registerPlugin",
        "-info", "{}",
    ], {
        cwd: DIR,
        stdio: ["ignore", "ignore", "pipe"],
        env: { ...process.env, AETHERSDR_TCI_PORT: String(tci.address().port) },
    });
    const stderr = [];
    child.stderr.on("data", (d) => stderr.push(d.toString()));

    const [deck] = await once(sd, "connection");
    deck.on("message", (d) => {
        const m = JSON.parse(d.toString());
        if (m.event === "setTitle") { titleWrites++; titles.set(m.context, m.payload.title); }
        else if (m.event === "setFeedback") feedback.set(m.context, m.payload);
        else if (m.event === "setImage") images++;
        else if (m.event === "getGlobalSettings") {
            deck.send(JSON.stringify({
                event: "didReceiveGlobalSettings", payload: { settings: {} },
            }));
        }
    });

    const toDeck = (o) => deck.send(JSON.stringify(o));
    const uuid = (a) => "com.aethersdr.radio." + a;

    const api = {
        sent, titles, feedback, stderr,
        imagesSent: () => images,
        titleWrites: () => titleWrites,
        appear: (a, ctx = a) => toDeck({ event: "willAppear", action: uuid(a), context: ctx }),
        disappear: (a, ctx = a) => toDeck({ event: "willDisappear", action: uuid(a), context: ctx }),
        press: (a, ctx = a) => toDeck({ event: "keyDown", action: uuid(a), context: ctx }),
        release: (a, ctx = a) => toDeck({ event: "keyUp", action: uuid(a), context: ctx }),
        rotate: (a, ticks, ctx = a) =>
            toDeck({ event: "dialRotate", action: uuid(a), context: ctx, payload: { ticks } }),
        pushDial: (a, ctx = a) => toDeck({ event: "dialDown", action: uuid(a), context: ctx }),
        fromRadio: (line) => radioWs && radioWs.send(line),
        // Drop the TCI link itself, so tciSend() has nowhere to write. Distinct
        // from `silent`, where the socket stays UP and the server simply does
        // not answer — a command still goes out in that case.
        dropTci: () => radioWs && radioWs.close(),
        settle: (ms = 250) => sleep(ms),
    };

    try {
        await sleep(300);      // init burst lands
        await body(api);
    } finally {
        child.kill();
        sd.close();
        tci.close();
        await sleep(50);
    }
    assert.deepEqual(stderr, [], "plugin wrote to stderr");
}

/**
 * Actions whose value is computed from a baseline the radio has to supply —
 * relative steps and cycles. These are the ones that must refuse until the
 * radio has reported. Absolute actions (band buttons, modes, PTT) are excluded
 * deliberately: they need no baseline and stay live from the first frame.
 */
const RELATIVE_DIALS = ["vfo-tune", "rf-power-dial", "tune-power-dial", "volume-dial"];

const RELATIVE_ACTIONS = [
    "tune-up", "tune-down", "band-up", "band-down",
    "rf-power", "tune-power", "volume-up", "volume-down",
    "slice-volume-up", "slice-volume-down",
    "nb-toggle", "nr-toggle", "anf-toggle", "apf-toggle", "sql-toggle",
    "mute-toggle", "split-toggle", "lock-toggle", "rit-toggle", "xit-toggle",
];

/** Boots the plugin against a TCI server that accepts the socket and is mute. */
async function withSilentRadio(body) {
    return withPlugin(body, { silent: true });
}

// Per-slice boolean flags the real init burst seeds for every slice, and which
// wireFlag() re-broadcasts on change. The fake radio has to report these or the
// command guard will (correctly) refuse the toggles that read them.
const FLAG_VERBS = [
    "rx_nb_enable", "rx_nr_enable", "rx_anf_enable", "rx_apf_enable",
    "sql_enable", "split_enable", "lock", "rit_enable", "xit_enable", "mute",
];

/** Mirrors the GET/SET split in TciProtocol::handleCommand: 0-1 args = GET. */
function answerGet(ws, record) {
    const colon = record.indexOf(":");
    const verb = (colon < 0 ? record : record.slice(0, colon)).trim();
    const args = colon < 0 ? [] : record.slice(colon + 1).split(",");
    // cmdVolume is the exception: it tests for an EMPTY argument list, so
    // "volume:" (one empty string) is a SET, and bare "volume" is the read.
    if (verb === "volume") {
        if (colon < 0) ws.send(`volume:${RADIO.volumeDb};`);
        return;
    }
    if (args.length >= 2) return;             // a SET
    const trx = args[0] || "0";
    if (verb === "drive") ws.send(`drive:${trx},${RADIO.drive};`);
    else if (verb === "tune_drive") ws.send(`tune_drive:${trx},${RADIO.tuneDrive};`);
    else if (verb === "rx_volume") ws.send(`rx_volume:${trx},${RADIO.rxVolume};`);
    else if (verb === "vfo") ws.send(`vfo:${trx},0,${RADIO.vfoHz};`);
    else if (FLAG_VERBS.includes(verb)) ws.send(`${verb}:${trx},false;`);
}

/** Every TCI command the plugin sent whose verb matches. */
const cmds = (sent, verb) =>
    sent.join(" ").split(";").map((s) => s.trim()).filter((s) => s.startsWith(verb + ":"));

// ── Manifest ────────────────────────────────────────────────────────────────

// Number of slice-target modes the plugin cycles through: TRX0, TX, ACTIVE.
const TARGET_MODE_COUNT = 3;

// Actions that deliberately send nothing to the radio: they change plugin-side
// selection or report connection state. Everything else declared in the
// manifest must reach the radio, or it is a dead button.
const LOCAL_ONLY = new Set([
    "com.aethersdr.radio.slice-target",
    "com.aethersdr.radio.tci-status",
]);

test("no declared action is a dead button", async () => {
    // Asked of the running plugin rather than scraped from the source: band and
    // mode handlers are generated from tables, so a source scrape has to
    // re-implement that generation and can silently under-count. Pressing each
    // action proves both that a handler exists and that it does something.
    await withPlugin(async (p) => {
        const dead = [];
        for (const action of manifest.Actions) {
            const name = action.UUID.replace("com.aethersdr.radio.", "");
            if (LOCAL_ONLY.has(action.UUID)) continue;
            const isDial = (action.Controllers || []).includes("Encoder");
            p.appear(name);
            await p.settle(30);
            const before = p.sent.length;
            if (isDial) {
                p.rotate(name, 1);
                await p.settle(120);
            } else {
                p.press(name);
                p.release(name);
                await p.settle(60);
            }
            if (p.sent.length === before) dead.push(action.UUID);
            p.disappear(name);
        }
        assert.deepEqual(dead, [], "declared actions that sent nothing to the radio");
    });
});

test("action UUIDs are unique and every action is categorised", () => {
    const uuids = manifest.Actions.map((a) => a.UUID);
    assert.equal(new Set(uuids).size, uuids.length, "duplicate action UUID");
    const uncategorised = manifest.Actions.filter((a) => !a.Category).map((a) => a.UUID);
    assert.deepEqual(uncategorised, [], "actions missing a Category group");
});

test("manifest Description states no action count", () => {
    // The count drifts every time an action is added and nobody updates the
    // prose; it was already stale once. Keep the phrasing open-ended.
    assert.ok(!/\d+\s+actions/i.test(manifest.Description),
        `Description must not state an action count: ${manifest.Description}`);
});

// ── Transmit safety ─────────────────────────────────────────────────────────

// PTT/MOX/TUNE always send trx:0, whatever the TX slice and whatever the
// selected target. The server discards the requested trx
// (TciRoutingState::resolvePttSlice returns the live TX slice, or a cached
// route), and only the requested != live branch refreshes that cache — so
// sending the TX trx would suppress the refresh and let a stale route from
// another client pick the slice. Pinned until #4547 lands.
test("PTT, MOX and TUNE always send trx:0, whatever the TX slice or target", async () => {
    await withPlugin(async (p) => {
        // Transmit moves to slice 1; GUI focus goes to slice 2. If the trx
        // tracked the radio these would be the values that leaked into the send.
        p.fromRadio("tx_enable:0,false;");
        p.fromRadio("tx_enable:1,true;");
        p.fromRadio("active_slice:2,C;");
        await p.settle();

        for (const a of ["ptt", "mox-toggle", "tune-toggle", "slice-target"]) p.appear(a);
        await p.settle();

        // Every target mode, including the two that do follow the radio.
        for (const _ of [0, 1, 2]) {                // TRX0 -> TX -> ACTIVE
            const before = p.sent.length;
            p.press("ptt"); p.release("ptt");
            p.press("mox-toggle");
            p.press("tune-toggle");
            await p.settle();
            const after = p.sent.slice(before);

            for (const c of cmds(after, "trx"))
                assert.match(c, /^trx:0,/, `PTT/MOX did not send trx:0: ${c}`);
            for (const c of cmds(after, "tune"))
                assert.match(c, /^tune:0,/, `TUNE did not send trx 0: ${c}`);
            assert.ok(cmds(after, "trx").length > 0, "no trx command was sent at all");
            assert.ok(cmds(after, "tune").length > 0, "no tune command was sent at all");

            p.press("slice-target");
            await p.settle();
        }
    });
});

// ── Slice targeting ─────────────────────────────────────────────────────────

test("slice-scoped actions follow the selected target", async () => {
    await withPlugin(async (p) => {
        p.fromRadio("tx_enable:0,false;");
        p.fromRadio("tx_enable:1,true;");
        p.fromRadio("active_slice:2,C;");
        p.appear("slice-target");
        p.appear("nb-toggle");
        await p.settle();

        const seen = {};
        for (const expected of [1, 2, 0]) {         // TRX0 -> TX -> ACTIVE -> TRX0
            p.press("slice-target");
            await p.settle(400);   // requestState()'s GET round for the new slice
            const before = p.sent.length;
            p.press("nb-toggle");
            await p.settle(150);
            seen[expected] = cmds(p.sent.slice(before), "rx_nb_enable")[0];
        }
        assert.match(seen[1], /^rx_nb_enable:1,/, "TX mode did not address the TX slice");
        assert.match(seen[2], /^rx_nb_enable:2,/, "ACTIVE mode did not address the focused slice");
        assert.match(seen[0], /^rx_nb_enable:0,/, "TRX0 mode did not address slice 0");
    });
});

// ── Protocol safety ─────────────────────────────────────────────────────────

test("the master-volume read never uses the SET-shaped `volume:` form", async () => {
    await withPlugin(async (p) => {
        p.appear("rf-power");
        await p.settle(400);
        // TciProtocol::cmdVolume tests for an empty argument list, but
        // "volume:" splits to one empty string — which is NOT empty — so the
        // colon form lands in the SET branch and drives master volume to 100%.
        const bad = p.sent.filter((s) => /(^|\s|;)volume:\s*(;|$)/.test(s));
        assert.deepEqual(bad, [], "sent the colon form of volume, which the server reads as SET 100%");
        assert.ok(p.sent.some((s) => /(^|;)\s*volume\s*(;|$)/.test(s)),
            "never issued the bare `volume` read");
    });
});

test("malformed records create no phantom state and no NaN labels", async () => {
    await withPlugin(async (p) => {
        for (const a of ["rf-power", "tune-power", "slice-volume-up"]) p.appear(a);
        await p.settle(400);
        const good = p.titles.get("rf-power");

        for (const junk of [
            "rx_nb_enable:,true;", "rx_nb_enable:abc,true;", "vfo:,0,14000000;",
            "drive:;", "rx_volume:x,50;", "active_slice:;", "tx_enable:,true;",
        ]) p.fromRadio(junk);
        await p.settle(300);

        for (const [ctx, title] of p.titles)
            assert.ok(!/NaN|undefined/.test(title), `${ctx} rendered "${title}"`);
        assert.equal(p.titles.get("rf-power"), good, "malformed input disturbed a good value");

        // The sharper failure is an unvalidated trx reaching the radio: a NaN
        // slice index would be sent as a literal, addressing nothing.
        p.appear("slice-target");
        p.appear("nb-toggle");
        await p.settle(150);
        const before = p.sent.length;
        for (let i = 0; i < TARGET_MODE_COUNT; i++) {   // every target mode
            p.press("slice-target");
            await p.settle(80);
            p.press("nb-toggle");
            await p.settle(80);
        }
        for (const c of p.sent.slice(before))
            assert.ok(!/NaN|undefined/.test(c), `sent a malformed command to the radio: ${c}`);
    });
});

// ── Radio-authoritative display ─────────────────────────────────────────────

test("keys show the radio's value, not a built-in default", async () => {
    await withPlugin(async (p) => {
        p.appear("rf-power");
        p.appear("tune-power");
        await p.settle(500);
        assert.match(p.titles.get("rf-power"), new RegExp(`${RADIO.drive} W`),
            "RF power key does not show the radio's power");
        assert.match(p.titles.get("tune-power"), new RegExp(`${RADIO.tuneDrive} W`),
            "tune power key does not show the radio's tune power");
    });
});

test("a press before the radio has reported commands nothing", async () => {
    // The display guard (`—`) and the command guard are separate: a key can
    // correctly show nothing while still stepping the radio from the struct's
    // initial value. Before this was gated, a cold-start Tune Down resolved to
    // 30 kHz and Tune Power to 50 W — the latter reaching TransmitModel even
    // with no radio connected, since cmdTuneDrive's SET path is not gated on
    // isConnected(). Driven against a server that accepts the socket and
    // answers nothing, which is the state every startup passes through.
    await withSilentRadio(async (p) => {
        for (const a of [...RELATIVE_ACTIONS, ...RELATIVE_DIALS]) p.appear(a);
        await p.settle(400);

        const before = p.sent.length;
        for (const a of RELATIVE_ACTIONS) { p.press(a); await p.settle(25); }
        p.rotate("vfo-tune", 1);
        p.rotate("rf-power-dial", 1);
        p.rotate("tune-power-dial", 1);
        p.rotate("volume-dial", 1);
        await p.settle(300);

        // requestState()'s GETs are reads and are expected; anything with a
        // value argument is this plugin inventing one.
        const writes = p.sent.slice(before).join(" ").split(";")
            .map((s) => s.trim()).filter(Boolean)
            .filter((c) => c.includes(",") || /^volume:-?\d/.test(c));
        assert.deepEqual(writes, [],
            "commanded the radio from a fabricated baseline before it had reported");
    });
});

test("once the radio reports, the same presses do command", async () => {
    // The guard must gate on knowledge, not disable the actions.
    await withPlugin(async (p) => {
        for (const a of RELATIVE_ACTIONS) p.appear(a);
        await p.settle(500);
        const before = p.sent.length;
        for (const a of RELATIVE_ACTIONS) { p.press(a); await p.settle(25); }
        await p.settle(200);
        const writes = p.sent.slice(before).join(" ").split(";")
            .map((s) => s.trim()).filter(Boolean)
            .filter((c) => c.includes(",") || /^volume:-?\d/.test(c));
        assert.ok(writes.length >= RELATIVE_ACTIONS.length - 1,
            `expected the gated actions to command once state is known, saw: ${writes.join(" ")}`);
    });
});

test("a key added later is painted, and a redraw is nudged for every key", async () => {
    await withPlugin(async (p) => {
        p.appear("rf-power");
        await p.settle(500);
        const imagesBefore = p.imagesSent();

        // Adding an action re-renders the whole deck from the manifest, so the
        // nudge has to reach every key rather than only the newcomer.
        p.appear("tune-power");
        await p.settle(900);

        assert.match(p.titles.get("tune-power"), new RegExp(`${RADIO.tuneDrive} W`));
        assert.match(p.titles.get("rf-power"), new RegExp(`${RADIO.drive} W`),
            "the existing key lost its value when another was added");
        assert.ok(p.imagesSent() > imagesBefore, "no redraw nudge was sent");
    });
});

test("an rx_smeter stream does not cause a repaint storm", async () => {
    await withPlugin(async (p) => {
        for (const a of ["rf-power", "tune-power", "tci-status", "slice-target"]) p.appear(a);
        // The willAppear repaint bursts are forced and the last fires at 2.5s,
        // so let them finish or they land inside the measurement window.
        await p.settle(2900);

        // TciServer::broadcastStatus emits rx_smeter per slice every 200ms to
        // every client. Repainting on each would be a continuous stream of
        // setTitle for values that never changed.
        // Count setTitle MESSAGES, not title values: a repaint storm re-sends
        // the same text, so comparing values would pass either way.
        const writesBefore = p.titleWrites();
        for (let i = 0; i < 30; i++) {
            p.fromRadio(`rx_smeter:0,${-90 - (i % 5)};`);
            p.fromRadio(`rx_smeter:1,${-95 - (i % 5)};`);
        }
        await p.settle(400);
        assert.equal(p.titleWrites(), writesBefore,
            `meter traffic caused ${p.titleWrites() - writesBefore} setTitle writes; `
            + "displayed values did not change, so none should have been sent");
    });
});

// ── Dials ───────────────────────────────────────────────────────────────────

test("the tune power dial drives tune_drive and never touches RF power", async () => {
    await withPlugin(async (p) => {
        p.appear("tune-power-dial");
        p.appear("rf-power-dial");
        await p.settle(400);

        const before = p.sent.length;
        for (let i = 0; i < 4; i++) { p.rotate("tune-power-dial", 1); await p.settle(8); }
        await p.settle(200);
        const after = p.sent.slice(before);

        assert.ok(cmds(after, "tune_drive").length > 0, "dial sent no tune_drive");
        assert.deepEqual(cmds(after, "drive"), [],
            "the tune power dial moved RF power — the two are separate radio settings");
    });
});

test("a dial ignores echoes mid-spin, then accepts them once settled", async () => {
    await withPlugin(async (p) => {
        p.appear("vfo-tune");
        await p.settle(400);

        // Spin while the radio repeats a stale value: without a guard the echo
        // overwrites ticks accumulated since the last send and the dial stalls.
        for (let i = 0; i < 5; i++) {
            p.rotate("vfo-tune", 1);
            p.fromRadio(`vfo:0,0,${RADIO.vfoHz};`);
            await p.settle(8);
        }
        await p.settle(200);
        const moved = p.feedback.get("vfo-tune").value;
        assert.notEqual(moved, (RADIO.vfoHz / 1e6).toFixed(3),
            "the dial was dragged back to the stale echo mid-spin");

        // Once the guard expires a genuine radio-side change must be taken.
        await p.settle(600);
        p.fromRadio("vfo:0,0,21074000;");
        await p.settle(250);
        assert.equal(p.feedback.get("vfo-tune").value, "21.074",
            "the dial ignored a radio change after the guard expired");
    });
});

// The echo guard was only ever exercised on the VFO dial, and the VFO dial was
// the only one it worked on: the other three had their backing field written
// DIRECTLY by parseTci before acceptEcho() could decline the value, so the
// guard was inert exactly where nobody was looking.
// `spun` is the value after FIVE +1 ticks with the guard working — i.e. the
// ticks accumulated. Asserting merely "it moved" is not enough to catch the
// bug: with the echo clobbering the backing field, every rotate still steps one
// notch up from the value the echo just wrote, so the dial appears to respond
// while never getting past the first step.
for (const d of [
    // 1 W per tick from the fixture's 37 W. Broken: stuck at 38 W.
    { action: "rf-power-dial",   stale: () => `drive:0,${RADIO.drive};`,
      spun: "42 W",  settled: "drive:0,88;",      settledShown: "88 W" },
    // 1 W per tick from 7 W. Broken: stuck at 8 W.
    { action: "tune-power-dial", stale: () => `tune_drive:0,${RADIO.tuneDrive};`,
      spun: "12 W",  settled: "tune_drive:0,44;", settledShown: "44 W" },
    // Volume steps across VOLUME_LEVELS by index; -6 dB is the fixture's 50%
    // (index 7), so +5 clamps to the top. Broken: stuck at 63%.
    { action: "volume-dial",     stale: () => `volume:${RADIO.volumeDb};`,
      spun: "100%", settled: "volume:0;",        settledShown: "100%" },
]) {
    test(`the ${d.action} ignores echoes mid-spin, then accepts them once settled`, async () => {
        await withPlugin(async (p) => {
            p.appear(d.action);
            await p.settle(400);

            for (let i = 0; i < 5; i++) {
                p.rotate(d.action, 1);
                p.fromRadio(d.stale());
                await p.settle(8);
            }
            await p.settle(200);
            assert.equal(p.feedback.get(d.action).value, d.spun,
                `${d.action} lost ticks to the stale echo mid-spin`);

            await p.settle(600);
            p.fromRadio(d.settled);
            await p.settle(250);
            assert.equal(p.feedback.get(d.action).value, d.settledShown,
                `${d.action} ignored a radio change after the guard expired`);
        });
    });
}

// ── Target changes driven by the radio ──────────────────────────────────────

test("a radio-driven target change re-seeds and repaints the new slice", async () => {
    await withPlugin(async (p) => {
        p.appear("slice-target");
        p.appear("slice-volume-up");
        p.appear("vfo-tune");
        await p.settle(400);

        // TARGET_MODES[0] is the fixed TRX0, which never follows the radio.
        // Step to "Slice TX" so the target tracks tx_enable.
        p.press("slice-target");
        await p.settle(300);

        // Slice 0 is the TX slice from the burst. Move TX to slice 1, which the
        // plugin has never read anything about.
        p.sent.length = 0;
        p.fromRadio("tx_enable:1,true;");
        await p.settle(400);

        // rx_volume is in neither the init burst nor any seed, so without a
        // re-request SLICE VOL+/- shows "—" and stays dead forever.
        assert.ok(p.sent.join(" ").includes("rx_volume:1"),
            "the newly targeted slice was never asked for its rx_volume");
        assert.ok(p.sent.join(" ").includes("vfo:1"),
            "the newly targeted slice was never asked for its frequency");

        // And the dial must not keep rendering the old slice's frequency.
        p.fromRadio("vfo:1,0,21300000;");
        await p.settle(300);
        assert.equal(p.feedback.get("vfo-tune").value, "21.300",
            "the VFO dial kept showing the previous slice's frequency");
    });
});

// ── Gating and dropped sends ────────────────────────────────────────────────

test("mute-all commands nothing before the radio has reported its volume", async () => {
    await withPlugin(async (p) => {
        p.appear("mute-all");
        await p.settle(300);
        p.sent.length = 0;
        p.press("mute-all");
        await p.settle(150);
        p.press("mute-all");
        await p.settle(200);
        // Un-gated, the unmute wrote radio.volume's fabricated 50% default.
        assert.equal(cmds(p.sent, "volume").length, 0,
            "mute-all commanded a volume the radio never reported");
    }, { silent: true });
});

test("an optimistic toggle does not flip when the send is dropped", async () => {
    await withPlugin(async (p) => {
        p.appear("tune-toggle");
        await p.settle(400);
        const before = p.titles.get("tune-toggle");

        // Take the link down. tciSend() now has nowhere to write and reports
        // failure, so the flag must not move.
        p.dropTci();
        await p.settle(300);

        p.press("tune-toggle");
        await p.settle(250);
        assert.equal(p.titles.get("tune-toggle"), before,
            "TUNE painted itself on over a radio that was never told");
    });
});

// ── TUNE state ──────────────────────────────────────────────────────────────

test("TUNE toggles off, and resyncs when stopped at the radio", async () => {
    await withPlugin(async (p) => {
        p.appear("tune-toggle");
        await p.settle(400);

        p.press("tune-toggle");
        await p.settle(150);
        let issued = cmds(p.sent, "tune");
        assert.match(issued.at(-1), /,true$/, "first press did not start TUNE");

        p.press("tune-toggle");
        await p.settle(150);
        issued = cmds(p.sent, "tune");
        assert.match(issued.at(-1), /,false$/, "second press did not stop TUNE");

        // There is no `tune:` broadcast, so a carrier stopped at the radio is
        // only observable through trx: going false.
        p.press("tune-toggle");
        await p.settle(150);
        p.fromRadio("trx:0,false;");
        await p.settle(200);
        p.press("tune-toggle");
        await p.settle(150);
        assert.match(cmds(p.sent, "tune").at(-1), /,true$/,
            "TUNE did not resync after being stopped at the radio — it needed two presses");
    });
});
