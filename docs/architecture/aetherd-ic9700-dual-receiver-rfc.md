# RFC — IC-9700 dual receiver, band ownership, and a radio-shaped RX applet

Status: **draft, for discussion.** Nothing here is built.

Companion to `aetherd-icom-civ-backend-design.md`, which describes the CI-V
backend as a whole. This one is about a single structural fact that document
does not yet handle: **the IC-9700 has three bands and only two receivers**, and
`IcomCivBackend` currently models one.

Evidence throughout is from live IC-9700 hardware — some from a night of testing
against the merged backend (2026-08-05), some from `aether-gate`'s IC-9700
adapter, which has driven this radio since 2026-06-30 and whose findings are
cited with their own dates. Where something is unverified it says so.

---

## 1. The problem, concretely

An IC-9700 covers 2 m, 70 cm and 23 cm. It has **two receivers**, and only one
of them can transmit.

`IcomCivBackend` today hardcodes a single receiver:

```cpp
[[nodiscard]] int sliceId() const noexcept { return 0; }
[[nodiscard]] QString panId() const { return QStringLiteral("0"); }
```

Meanwhile `kModels` declares `receivers: 2`, and `capabilities()` faithfully
publishes `maxSlices = 2`, `maxPanadapters = 2`. **So the app advertises two
receivers and the backend materialises one.** Every symptom below follows from
that gap.

### What that produces, observed

Adding 2 m / 440 / 23 cm band buttons (they did not previously exist — the band
grid is an HF Flex grid topping out at 2 m, and even that is gated on a Flex
model-string flag) made the buttons appear correctly. Pressing them does not
tune:

```
23:14:28.024  MainWindow: switching to band "2m" freq: 144.2 mode: "USB"
23:14:28.025  MainWindow: band switch (no radio band stack) band=2m … freq_mhz=144.200000
                          … and no CI-V traffic at all
```

The slice moves to 144.200 in the model; the radio stays on 432; the waterfall
keeps painting 432. The display ends up lying about where the radio is.

The cause is not in AE's dispatch. It is that **`25 00 <freq>` cannot cross
between receivers.** aether-gate found this on hardware and works around it:

> *"If a MAIN tune is FA'd because the target band is parked on the SUB
> receiver, the LAN tuner simply gives up (cross-band tuning is the USB
> channel's job); it does NOT swap."*
> — `aether_gate/adapters/icom/icom9700.py`

`FA` is the radio's NAK. The command goes out, the radio refuses it, and nothing
in AE notices — `setSliceFrequency` is fire-and-forget.

### Why "just send a band-select" is the wrong fix

The obvious patch is to issue the receiver swap (`07 B0`) when the target band
is on the other receiver. That works, and it is a trap:

> *"⚠ SINGLE-SWAPPER RULE: the LAN channel must NEVER issue 07 B0. The swap is
> GLOBAL to the radio (proven 2026-07-03) — two masters swapping (LAN tuner +
> USB RX2) collide and tangle MAIN/RX2."*
> — same file

A swap is not a per-client operation. It re-points the radio for everyone
attached to it. Building band changes on an unconditional swap means every band
press is a global side effect, and two clients doing it collide.

**The framing that avoids this**: with two receivers and three bands, most band
changes should not swap at all. If both receivers are already open on known
bands, "go to 70 cm" is a *slice selection*, not a retune. Only a band on
neither receiver needs anything re-pointed — and then exactly one receiver
should move, chosen deliberately.

---

## 2. Proposal

### 2.1 Probe the receiver count at connect

aether-gate already does this, proven on hardware:

```
07 B0            swap MAIN <-> SUB
25 00 / 26 00    read the now-selected receiver's freq + mode
07 B0            swap back
```

> *"Sets rx2_present True iff RX2 read a real ham freq on a DIFFERENT band than
> MAIN (a genuine second receiver, not RX1's own VFO B)."*

The different-band test is the load-bearing part: without it, VFO B on the same
band reads as a second receiver.

**Open one pan or two accordingly.** One receiver → one pan, exactly as today.
Two → two pans, each seeded with the frequency and mode its receiver reports.

⚠ **Probe once, at connect, and never on a timer.** The swap briefly yanks the
live scope to the other receiver and back; gate's notes are explicit that this
must be on-demand only, *"NEVER on the periodic poll, because the swap briefly
yanks the live scope to RX2 and back (would worsen the scope-stall)."* At
connect the scope is not yet up, which is the one moment the disturbance costs
nothing.

### 2.2 TX ownership follows the existing Flex model

No new concept needed. A Flex already has exactly one TX slice among several,
and AE already models that. The 9700's constraint — only MAIN transmits — maps
onto the existing TX-slice flag directly.

The rule this RFC asks for explicitly:

> **A band change never moves the TX receiver implicitly.** If the requested
> band is on neither receiver, the NON-TX receiver is the one that moves. Moving
> the TX receiver is an explicit operator action with its own affordance.

This is a safety property, not a preference. A band button that can silently
re-point the transmitting receiver is a band button that can leave the operator
keying on a band they are no longer looking at.

### 2.3 Band selection is per-pan, and offers only what is free

Right-click a pan → choose its band. The menu offers only bands **not currently
held by the other receiver**, because the radio cannot put both receivers on the
same band and there is no reason to offer an action that will fail.

This is better than the band-button row for this radio: it puts the choice where
the constraint actually lives (per receiver) instead of implying three
independent bands. The band buttons remain meaningful for the *active* pan.

### 2.4 An RX applet shaped like the radio

The question that prompted this: with two pans, which do you show?

"Show only the TX pan, with a button to transfer TX ownership so you can see the
other one" conflates two things that should never be coupled — **what you are
looking at** and **what will transmit**. Tying them means changing what is on
screen changes what is on the air.

Instead: show both pans, and give the second receiver **its own applet** — a
compact panel on the right, laid out like the radio's own sub-band controls:

- audio level (concentric: inner/outer, as on the rig)
- squelch
- the receiver's frequency and mode
- an explicit, clearly-marked TX-ownership control

There are already 35+ applets in `src/gui/` and `AppletPanel` is the established
host, so this is a conventional addition rather than new infrastructure.

The virtue is that TX ownership becomes **one deliberate control in one place**,
rather than an implicit consequence of which pan you happened to select. That is
the same reasoning behind the explicit-arm TX pattern already used elsewhere in
the shack.

---

## 3. Work implied

Deliberately ordered so nothing on-air changes until the groundwork is proven.

1. **Make the backend receiver-aware.** `sliceId()` / `panId()` become
   per-receiver rather than constants. No behaviour change with one receiver;
   this is the foundation everything else needs and is independently reviewable.
2. **Connect-time receiver probe** (§2.1) → open one pan or two.
3. **Cross-band routing** (§2.2) — including *detecting the FA* rather than
   firing and forgetting, which is a gap in `setSliceFrequency` today regardless
   of this RFC.
4. **Per-pan band menu** (§2.3).
5. **Second-receiver applet** (§2.4).

Steps 1–2 are worth doing on their own merits: they make the backend match the
capabilities it already advertises.

---

## 4. Open questions

1. **Coexistence with aether-gate.** The gate currently owns the swap on its USB
   channel precisely because the swap is global. If AE natively drives two
   receivers, what happens when both are attached? Options: document
   "don't run both", have AE detect the gate, or leave AE swap-free and let it
   only *select* among receivers the operator has already set up. The last is
   the most conservative and may be enough for most operating.
2. **Does anything else share this shape?** The IC-905 is multi-band with
   limited receivers; if so, this should be a capability rather than an IC-9700
   special case — `receivers` already exists in `IcomModel` and may be sufficient.
3. **Is the swap safe during TX?** Not tested. Until it is, the answer should be
   "no swap while keyed", enforced rather than assumed.
4. **What does the scope do across a swap?** Gate's notes say it yanks and
   returns. With two pans, does the second pan get scope data at all, or is the
   scope MAIN-only? This gates whether pan 2 is a waterfall or a frequency
   readout. **Needs the radio.**

Question 4 is the one that most changes the design, and it is an hour on the
bench.

---

## 5. Relationship to aetherd and "one AE, many radios"

Checked deliberately, because a proposal that assumes one radio would be the
wrong shape against a design where several are normal.

**It fits, and mostly for free.** `aetherd-headless-engine-design.md` §5 already
establishes per-radio `RadioSession` with the protocol namespaced by session id,
*"so one daemon can host several radios — of the same or different families —
concurrently."* Everything in this RFC lives **inside** one session: two
receivers of one IC-9700, not two radios. Nothing here reaches across sessions,
so the multi-radio story is unaffected either way.

The staged plan's step 2 (the `IRadioBackend` seam) has landed, and its own note
says *"from this point a new radio family is a new backend — shippable in-process
in the desktop app immediately, and served through the daemon automatically once
the later steps land."* Work here is behind that seam, so it inherits the daemon
for free rather than needing porting later.

Three places the alignment is load-bearing rather than incidental:

- **§2.1's connect-time probe is per-session state**, not global. Two IC-9700s on
  one daemon each probe their own receivers. Nothing in the probe reads or writes
  anything session-external — worth stating because the `07 B0` swap *is* global
  to its radio, which makes "global" an easy word to get wrong here. It is global
  to that radio, not to the engine.
- **§2.2's TX rule strengthens under multi-client.** §6 of the aetherd RFC makes
  TX arbitration engine-side and non-negotiable (Principle VI). "A band change
  never moves the TX receiver implicitly" is the same principle one level down:
  with several clients projecting one session, an implicit TX move would be
  invisible to every client that did not initiate it.
- **§2.4's applet is client-side.** Under aetherd it is a projection, so a second
  client would render its own. The TX-ownership control it carries is a *request*
  to the engine, arbitrated there — not a local mutation. That is the correct
  split already and needs no rework.

**The one open question multi-radio adds** to §4's list: the single-swapper rule
is currently about AE-vs-aether-gate, but under a daemon with several clients it
generalises. If two clients on one session both request a band that needs a swap,
the swap must serialise at the engine — the same shared-mutation question §5 of
the aetherd RFC already flags for pan/slice creation. Recommend it be treated as
a shared mutation, announced to all clients, exactly as that section proposes.

---

## 6. What this does not propose

- No change to TX behaviour beyond making ownership explicit. Nothing here keys
  a radio, and step 3 should land with TX untouched.
- No transverter/XVTR interaction. The gate publishes 70 cm and 23 cm as XVTR
  bands *because* AE had no native buttons; with native bands that workaround
  should become unnecessary, but removing it is out of scope here.
- No claim that the MOD Input / TX-audio path works. That is unresolved
  separately (the `1A 05` menu item numbers are model-specific and unverified on
  this model).
