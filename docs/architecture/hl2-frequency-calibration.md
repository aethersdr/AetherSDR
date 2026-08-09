# HL2 manual frequency calibration — design note

Status: implemented. `Hl2FreqCal`, the Calibration page in `RadioSetupDialog`,
and the `freqcal` bridge verb.

The Hermes-Lite 2 tunes off a free-running crystal oscillator. Nothing in the
radio can be told about its own error, so every correction has to happen on this
host. This note establishes what the hardware does and does not offer, derives
the correction, picks the seam, and proposes the operator-facing control.

---

## 1. What the hardware gives us — and what it does not

### The clock chain

Confirmed at tier 1 (gateware Verilog) and tier 2 (HL2 wiki), per the
source-precedence ladder in `~/oracles/hl2/hl2-oracle.md` §0:

```
38.4 MHz oscillator  →  5P49V5923 VersaClock  →  76.8 MHz  →  AD9866
                        FB_intdiv = 0x044             (÷2 ÷0x011)
```

`sources/wiki/External-Clocks.md`: the FPGA state machine programs the
VersaClock at power-on to multiply 38.4 MHz to a 2611.2 MHz VCO, then divides to
76.8 MHz. **The wiki explicitly notes fractional division is available but
deliberately unused, "to keep jitter to a minimum."**

The 38.4 MHz part is a plain oscillator on most builds, not an oven or a TCXO.
Its error is what we are correcting. Field reports put a good unit around
0.2 ppm; the part tolerance allows an order of magnitude worse, and it walks
with temperature and warm-up.

### The NCO is a fixed-point multiply against a compile-time constant

`sources/gateware/gateware/rtl/radio_openhpsdr1/radio.v`:

```verilog
parameter  CLK_FREQ = 76800000;
// B57 = 2^57.   M2 = B57/OSC
localparam M2 = ... (CLK_FREQ == 76800000) ? 32'd1876499845 : ...;
localparam M3 = 32'd16777216;          // 2^24, for rounding
...
assign freqcomp = cmd_data * M2 + M3;  // line 321
...
freqcompp[0] <= freqcomp[56:25];       // >> 25 → the 32-bit phase word
```

So the phase increment is `(f_Hz · 2^57/76 800 000 + 2^24) >> 25`, and
`76 800 000` is **baked into the bitstream**. The same `freqcomp` feeds both
`rx_phase[]` and `tx_phase0` (lines 397–413), so RX and TX share one scale
factor — a correction applied at the command boundary fixes both by
construction.

Phase-word resolution: 76.8 MHz / 2³² = **0.0179 Hz**. Not the limiting factor.

### There is no calibration register anywhere

The full HL2 register map (`sources/wiki/Protocol.md`) exposes frequencies as
plain unsigned Hz at `0x01` (TX1 NCO) and `0x02`–`0x08`, `0x12`–`0x16` (RX
NCOs). There is no ppm field, no clock-frequency field, no trim. The HL2's own
tool (`software/hermeslite/hermeslite.py`) has no calibration function either.
The openHPSDR P1 `10 MHz Ref` / `122.88 MHz source` selector bits in `0x00`
belong to Atlas-bus hardware and are not decoded by HL2 gateware.

**Conclusion: host-side correction is the only software option.** This is also
what the reference clients do — Quisk's hermes layer carries the *measured*
clock as `conf.rx_udp_clock` and reconciles it against the FPGA's nominal
constant in `Freq2Phase()` / `ReturnVfoFloat()`
(`sources/quisk/hermes/quisk_hardware.py:482–495`).

---

## 2. The correction, derived

Let the true master clock be `f_c = 76.8 MHz · (1 + e)`, `e` the fractional
error. Let `U` be the frequency the operator dialled and the app commanded.

1. Hardware LO: `f_LO = (phase/2³²) · f_c = (U / 76.8e6) · f_c = U·(1 + e)`.
2. A signal at true RF `F` lands at real baseband `b = F − U(1+e)`.
3. Samples arrive at `48 000·(1 + e)` but every DSP stage — the software shift,
   WDSP, the panadapter axis — labels them `48 000`. A real offset `b` therefore
   *reads* as `b / (1 + e)`.
4. Displayed absolute frequency:
   `U + b/(1+e) = U + (F − U(1+e))/(1+e) = F/(1 + e)`.

**The entire displayed frequency scale is off by exactly `1/(1+e)`, independent
of where the NCO sits or how much software shift is in play.** One
multiplicative scalar corrects everything. That is the load-bearing result: it
means we do *not* need a per-band table, and it means the two-stage
NCO-plus-shift tuning the HL2 backend uses does not complicate the fix.

Define

```
k = 1 / (1 + e)          # the scale applied to every commanded frequency
```

To make a dialled `U` land on true `U`, command `U·k`.

### Measuring `e`

Zero-beat a reference of known true frequency `F_ref` (WWV 10 MHz). Read the
dial value `U₀` at which it zero-beats. From step 4, `F_ref/(1+e) = U₀`, so:

```
1 + e = F_ref / U₀
k     = U₀ / F_ref
ppb   = (F_ref/U₀ − 1) · 1e9
```

Read plainly: **`k` is "the frequency you had to dial, over the frequency it
actually is."** A clock running fast makes signals appear *low*.

Sanity check against the wiki: with a GPSDO the HL2 reads 10 MHz dead on; on its
internal crystal one unit read `10 000 001.78 Hz` for WWV, i.e. signals appeared
1.78 Hz *high* ⇒ `e = −178 ppb`, clock slow. Consistent.

### Applying `e` at the seam

`Hl2Backend::setSliceFrequency()` splits a tune into a hardware NCO plus a
software shift. Correcting both, and letting the shift absorb the NCO's integer
rounding, is exact:

```
nco_cmd   = llround(nco_true · k)                 # uint32 Hz → register 0x02+
shift_cmd = slice_true · k − nco_cmd              # → Hl2RxDsp::setShift()
```

Realised frequency `= (nco_cmd + shift_cmd)·(1+e) = slice_true·k·(1+e) =
slice_true`. The 1 Hz quantisation of the NCO register cancels out entirely,
because the shift is computed against the rounded value rather than the ideal
one. Do not compute `shift_cmd` from `nco_true`.

TX is single-stage — `Hl2Backend::setTxFrequency()` puts the carrier straight on
register `0x01` with no shift — so its floor is the 1 Hz register granularity.
At 10 ppm on 28 MHz we are correcting a 280 Hz error down to sub-Hz. Fine for
WSPR and FT8.

### What is deliberately left uncorrected

| Quantity | Error at 10 ppm | Verdict |
|---|---|---|
| Wideband spectrum bin width (76.8 MHz / 2048 ≈ 37.5 kHz) | ~0.4 Hz per bin | Invisible. Ignore. |
| Audio/IQ stream rate (48 k … 384 k) | 0.48 Hz at 48 k | Absorbed by existing resampling. Not a tuning concern. |
| Panadapter axis | corrected implicitly — the axis is drawn from `ncoHz`, which stays in the true-RF domain | — |

Keep `Receiver::ncoHz` and `sliceFreqHz` in the **true-RF domain**. Scale only
where a number leaves for the radio. The pan-centre emit at
`Hl2Backend.cpp:3773` then needs no change.

---

## 3. Rejected alternative: trim the VersaClock

The 5P49V5923 has a 24-bit fractional feedback multiplier (registers
`0x19`–`0x1B`), reachable over the protocol's I2C bridge at address `0x3c`
(cookie `0x06`, chip `0xd4`). Resolution works out to ~0.07 Hz on the 76.8 MHz
output — about **0.9 ppb**, more than sufficient. It would also fix the sample
rate, which the host-side scalar does not.

Rejected anyway:

- The HL2 project avoids fractional division *by design* for jitter. Trading
  phase noise on every receiver for a correction that a host-side scalar makes
  exactly is a bad trade.
- The clock must never stop; the CL1 switching recipe exists precisely because
  glitching it can leave the AD9866 PLL unlocked and the FPGA clockless. A
  mis-write here bricks the radio until power-cycle.
- It is invisible to everything else. Every readback and every other client
  would still assume 76.8 MHz.

Not worth it. The scalar is exact for tuning, which is the whole requirement.

---

## 4. A later, different feature: CL1 external 10 MHz

**A GPSDO does not require CL1 to be useful here.** Its 10 MHz output is a
calibration *signal* — receive it, read where it lands, enter the number. That
is the §5 workflow verbatim, and a local GPSDO is a strictly better reference
than WWV: no ionospheric Doppler (which wanders ~10 ppb at 10 MHz), no fading
mid-null, enormous SNR. Its harmonics at 20 and 30 MHz are also a free
validation of the scalar model — calibrate on 10 MHz, then confirm 30 MHz nulls
with the same ppb rather than needing its own value.

Two cautions for the UI text: a GPSDO's output (typically +7…+13 dBm) will
overload the AD9866, which has no attenuator ahead of it beyond the LNA range —
pad it ≥30 dB or couple loosely. An overloaded ADC is a *correctness* problem
here, not only a safety one: the spurs it generates can land near the carrier
being measured.

What CL1 buys that manual calibration does not is **drift**. A manual number is
a snapshot of a plain crystal that moves several ppm through warm-up and with
ambient temperature. `sources/wiki/External-Clocks.md` documents a working
recipe for driving CL1 from a 10 MHz GPSDO (multiply by 288 → 2880 MHz VCO,
divide by 18.75×2 → 76.8 MHz), which removes the error permanently rather than
modelling it once. Worth doing for all-day WSPR/FT8; not needed for "the dial
should read right." Separate PR — it needs the I2C write path at `0x3c`, the
switch-without-stopping sequence, and a lock indicator.

Two hard constraints if we build it:

- **Hardware damage risk.** Many GPSDOs put out a 5 V sine; CL1 wants a 3.3 V
  square wave. The wiki requires ≥6 dB of 50 Ω SMA attenuation. Any UI that
  offers this must say so, in the UI, not just the release notes.
- When CL1 is locked, the manual ppb from this feature must be forced to zero
  and the control disabled — otherwise we would correct an already-correct
  clock.

This is also the natural HL2 counterpart to the Flex **10 MHz Reference** group
that already exists in `RadioSetupDialog::buildRxTab()`.

---

## 5. Proposed operator surface

### Where

A new **Calibration** page under the **RADIO** category in `RadioSetupDialog`,
hidden unless the connected backend declares it.

Gate on a **capability, not a family name**. `RadioCapabilities.h` is explicit
about this ("must be a capability, not a family-name special case"). Add:

```cpp
// The radio tunes off an uncalibrated local oscillator and cannot be told its
// own error, so the CLIENT applies a frequency-scale correction on every
// commanded frequency. True for the HL2 (free-running 38.4 MHz crystal, no
// calibration register in the protocol); false for a radio that owns its own
// reference and its own calibration command (Flex: `radio set freq_error_ppb`).
bool hostFrequencyCalibration = false;
```

Set it in `Hl2Backend`, leave it false in `FlexBackend` and `SimBackend`, and
record it in `docs/architecture/radio-capabilities-map.md`. Follow the existing
`apdItem->setHidden(...)` pattern at `RadioSetupDialog.cpp:~770` for the
show/hide.

### Why not just +/- buttons

Stepping alone was the first instinct and it is half right. Nudge buttons are
genuinely the best control *while you are listening to a beat note* — you want
your hand on something that changes the pitch in real time. But they are a poor
way to *hold* a calibration:

- You cannot type in the number a GPSDO comparison or a previous session gave
  you.
- You cannot read your value back, write it down, or reproduce it on a rebuild.
- Steps in Hz are wrong-headed: the error is fractional. A step that means 1 Hz
  on 10 m means 0.1 Hz on 160 m. The stored quantity has to be ppb (or a
  scalar); only the *step* should be expressed in Hz.

So: keep the nudge, but as one of three ways into a single stored number.

### The page

**Group: Frequency Calibration** *(mirrors the Flex "Frequency Offset" group's
vocabulary — that group already speaks in `freq_error_ppb`)*

| Control | Behaviour |
|---|---|
| **Reference** — combo | WWV/WWVH 2.5 / 5 / 10 / 15 / 20 / 25 MHz, CHU 3.330 / 7.850 / 14.670 MHz, Custom… Default 10 MHz. **Custom is not an afterthought** — a bench GPSDO or a locked signal generator is the best reference available (§4), and it may sit anywhere. |
| **Calibrate from current VFO** — button | Reads the active slice's dialled frequency `U₀`, computes `ppb = (F_ref/U₀ − 1)·1e9`, applies and persists. The operator zero-beats with the normal tuning knob they already know, then presses one button. This is the primary path. |
| **Trim** — `−` / `+` with a step combo | Steps of 0.1 / 1 / 10 Hz-at-10-MHz (= 10 / 100 / 1000 ppb). Applied live so the beat note moves under the operator's hand. |
| **Error** — spinbox | Direct ppb entry, range ±50 000 ppb (±50 ppm), 1 ppb granularity. For a value you already know. |
| **Reset** — button | Back to 0 ppb. |
| Derived readout | "Effective clock 76 799 986 Hz · −182 ppb · −5.2 Hz at 28.500 MHz" — recomputed as the operator's current slice moves, so the correction is shown in the units they actually care about. |
| Warning line | "Let the radio warm up for 15 minutes before calibrating. The HL2's oscillator drifts with temperature." |

Range rationale: ±50 ppm comfortably covers a bad crystal; a stock unit lands
inside ±10 ppm. 1 ppb granularity is 0.01 Hz at 10 MHz — far finer than anyone
can zero-beat, but it costs nothing and keeps a GPSDO-derived number exact.

### Persistence

**Keyed per radio (MAC), not globally.** The number describes one physical
crystal. An operator with two HL2s must not have the second one's calibration
silently applied to the first. That rules out `Hl2Settings` — its `Hl2` root
object is one document for the family — so the value is a **radio-scoped feature
document** (RFC #4603), the same mechanism `Hl2Discovery` uses for per-radio
nicknames:

```
radio_settings (family='hl2', radio_id=<MAC>, feature='Calibration')
    { "freqErrorPpb": <int> }        # Hl2FreqCal::kFeature / kFieldPpb
```

`Hl2FreqCal::loadPpb()` / `savePpb()` are the only readers and writers. Two
consequences that are load-bearing rather than incidental:

- **0 ppb removes the document** instead of storing a zero, so "reset" means
  never-calibrated rather than a row that reads the same but would shadow a
  future family-wide default.
- **An empty `radio_id` is never written.** `RadioSettingsScope` falls back
  exact-radio → family-wide on read, so a row stored with no identity would be
  inherited by every HL2 that has none of its own — precisely the contamination
  the per-MAC key exists to prevent. `Hl2Backend::applyFreqCalPpb()` refuses and
  warns, `AutomationServer::doFreqCal()` returns an error, and the Calibration
  page disables its controls with an explanatory line. All three refuse rather
  than reporting a success nothing persisted.

---

## 6. Follow-ups worth scoping separately

1. **CL1 external reference** (§4) — the only one of these that solves a problem
   manual calibration cannot, namely drift.
2. **Automatic measurement** — *optional, low priority.* Find the reference
   carrier's FFT peak, interpolate, compute ppb: the equivalent of Flex's
   `radio pll_start`. It removes the imprecision of nulling by ear, but that
   imprecision is already small and there is a simpler manual route that needs
   no new code — tune the reference in CWU at a 500 Hz pitch and read the audio
   tone. A tone at 505 Hz is a 5 Hz error, i.e. 500 ppb at 10 MHz. Build this
   only if operators ask for it.
3. **Transverters.** HL2 has no transverter path today (AetherSDR's XVTR support
   is Flex-side, driven by `sub xvtr all`). If one is added, the calibration
   applies to the **IF**, not the RF — it is the HL2's clock that is wrong, not
   the transverter's LO, and those are two independent errors with two
   independent corrections. Quisk gets this right
   (`int(vfo_freq - transverter_offset)`).

## 7. Automation bridge verbs

Per the standing rule that new features come with automation coverage. One verb
with sub-actions rather than four top-level verbs, matching how `tune`, `gps`
and `waveform` are already registered:

```
freqcal                        → {"ppb":0,"ppm":0,"effectiveClockHz":76800000}
freqcal set -178               # set and persist
freqcal from_vfo 10.0          # derive ppb from the active slice (MHz reference)
freqcal reset
```

Gated on `RadioCapabilities::hostFrequencyCalibration`, so it refuses on a radio
that calibrates its own reference rather than storing a number nothing applies.
The mutating actions also refuse before the first connect, for the §5 reason.
A change takes effect immediately — every RX NCO, every DSP shift and the TX
oscillator are re-pushed — so a script can calibrate and tune without a
reconnect.

Internally these route to the backend's `hl2` extension namespace
(`freqcal.get` / `freqcal.set` / `freqcal.set_live`) through
`RadioModel::invokeBackendExtension`, and `Hl2Backend` declares `"hl2"` in
`capabilities().extensionNamespaces` so the capability handshake reports the
namespace it actually implements. `freqcal.set_live` applies and re-pushes
without touching the settings store —
it exists for the Calibration page's auto-repeating Trim buttons, which commit
once when the button is let go.

The test that matters is not "does the setting round-trip" — it is that a
commanded frequency reaches the wire scaled. Assert on the bytes in the `0x02`
and `0x01` command banks, not on the model value: **a convention error is
invisible to any test that shares the convention.** Command 10 000 000 Hz at
+1000 ppb and require `0x02` to carry 9 999 990, and require the TX bank at
`0x01` to carry the same scaling. Both banks, both directions.
