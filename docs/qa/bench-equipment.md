# Bench equipment for gateware and wire-level validation

A bill of materials for developers who validate what the HL2, ANAN and G2
backends — and the gateware behind them — actually put on the wire and in the
air. It is organised by what each item lets you **prove**, not by price.

> **Scope and dating.** Written 2026-11 for the radios this project supports over
> openHPSDR: Hermes-Lite 2 (≈5 W, 0–38.4 MHz), ANAN-G2 / Saturn (100 W,
> HF + 6 m) and classic Orion MkII boards (100–200 W). Model names are
> **illustrative examples, not endorsements** — check current specifications
> and prices before buying. Cost bands are rough: **$** < $100, **$$** $100–500,
> **$$$** $500–2,000, **$$$$** > $2,000.
>
> Design background for the measurements below — DDC/DUC chains, filter taps,
> CW shaping, PureSignal routing — is in
> [`openhpsdr-gateware-design-internals-2026-11-05.md`](https://github.com/aethersdr/aether-fabric/blob/main/docs/research/openhpsdr-gateware-design-internals-2026-11-05.md)
> in the [`aether-fabric`](https://github.com/aethersdr/aether-fabric) repository (private for now).

---

## 1. Safety rules first

These restate rules the project already applies to transmit work
([`docs/CERTIFICATION.md`](../CERTIFICATION.md),
[`anan-p2-backend-design.md`](../architecture/anan-p2-backend-design.md)); the
equipment list exists to make them easy to follow.

1. **Dummy load for every keyed stage. No antenna.**
2. **External PA out of line** — disconnected or in standby for the whole session.
3. **Nothing with a receiver input ever sees transmit power directly.** A spectrum
   analyzer, SDR or scope connects only through a sampler/coupler **and**
   attenuation sized by the level budget in §2. A monitor receiver a few inches
   from a dummy load can saturate on its own ("even at 5 % drive",
   [`CERTIFICATION.md`](../CERTIFICATION.md)) — give it inline attenuation, not
   just distance.
4. **Clamp TX power in software** (`AETHER_AUTOMATION_TX_MAX_POWER`) and start
   every keyed test at the lowest drive.
5. **Mind the heat and the duty cycle.** Dry dummy loads are often rated for
   seconds-to-minutes at full power; long two-tone or digital-mode tests need a
   load rated for **continuous** power at that level.
6. **RF exposure and burns.** Keep hands off coax and loads while keyed; a
   "small" 5 W HL2 still burns at a bad connector.

---

## 2. The two measurement chains

### Transmit chain

```
Radio ─▶ directional wattmeter ─▶ RF sampler (−40…−50 dB tap) ─▶ dummy load
                                         │
                                  sample port
                                         ▼
                     step / fixed attenuator ─▶ DC block + limiter ─▶ spectrum analyzer
                                                                   └▶ monitor SDR / scope
```

### Level budget — size the attenuation before keying

| Radio output | dBm at the radio | After 40 dB tap | Pad for ≈ −20 dBm at the analyzer |
|---|---|---|---|
| HL2 5 W | +37 dBm | −3 dBm | 15–20 dB |
| G2 / ANAN 100 W | +50 dBm | +10 dBm | 30 dB |
| Orion MkII 200 W | +53 dBm | +13 dBm | 30–35 dB |

Most bench spectrum analyzers list an absolute-maximum input around +20 to
+30 dBm and measure cleanly with the input mixer near −10 to −30 dBm; check the
specific instrument. Aim low: an overdriven analyzer manufactures exactly the
harmonics and IMD you are trying to measure. Two-tone and PEP peaks run up to
6 dB above the average-power reading.

### Receive chain

```
Signal generator A ─▶ 6 dB pad ─┐
                                ├─▶ hybrid combiner ─▶ step attenuator ─▶ radio RX
Signal generator B ─▶ 6 dB pad ─┘    (≥ 25 dB isolation)   (0–110/120 dB)
        ▲
   shared 10 MHz reference (GPSDO) ─── also to the analyzer and the radio
```

Run sensitivity and IMD work inside a shielded box, or at least with the
radio, attenuator and generator screened from switching supplies and the PC.

---

## 3. Bill of materials

### Tier 1 — essential (anyone touching transmit, meters or gateware I/O)

| Item | Key specification | What it lets you prove | Examples | Cost |
|---|---|---|---|---|
| **Dummy load** | 50 Ω, SWR < 1.1:1 to 54 MHz; **continuous** rating ≥ the radio (≥ 25 W for HL2, ≥ 250 W for 100–200 W boards); published duty-cycle curve | Every keyed test, safely; SWR reads 1.0:1 (the HL2 SWR-formula check in [`HERMES.md`](../HERMES.md) was validated exactly this way) | Dry loads for the HL2; oil- or air-cooled loads with a duty-cycle chart for 100 W+ | $–$$$ |
| **In-line RF sampler / directional coupler** | 40–50 dB coupling, flat within ±0.5 dB over 1.8–54 MHz, power rating ≥ radio | A known, attenuated copy of the TX signal for the analyzer and monitor receiver; PureSignal feedback-level experiments | Commercial or kit HF samplers; Mini-Circuits-style directional couplers at low power | $–$$ |
| **Fixed attenuators + DC block + RF limiter** | SMA/N, 10/20/30 dB, power-rated for their position in the chain | Analyzer protection; building the level budget in §2 | Mini-Circuits VAT-series at low power; high-power attenuators directly after the sampler | $–$$ |
| **Spectrum analyzer** | 9 kHz to ≥ 1 GHz (harmonics of 54 MHz out to 10×); RBW down to ≤ 100 Hz; tracking generator option | TX harmonics and spurs, two-tone IMD, CW key-click sidebands, DUC images, reference-loop spurs; filter sweeps with the tracking generator | Siglent SSA3000X Plus series; Rigol DSA800 series; tinySA Ultra as a budget harmonics/spur checker (limited dynamic range — not for IMD) | $$–$$$ |
| **Independent monitor receiver** | Good HF dynamic range; fed through attenuation | Hear what actually left the radio — sideband and I/Q-inversion checks, CW timing, audio quality — without trusting the radio's own meters | SDRplay RSP1B (already used on the G2 bring-up); Airspy HF+ Discovery; RTL-SDR Blog V4 (budget) | $–$$ |
| **HF wattmeter** | Accurate forward/reflected, peak reading, 1.8–54 MHz | Validate `TX:FWDPWR`/`TX:REFPWR`/SWR scaling against a known reference — the meter-scale class of bug in [`CERTIFICATION.md`](../CERTIFICATION.md) | TelePost LP-100A (RS-232, drivable — §6); Keysight U2004A USB sensor behind the sampler for calibrated absolute power. A Bird 43 is a fine manual cross-check but cannot be driven | $$–$$$ |
| **Bench power supply** | 13.8 V with current readout and limit; ≥ 25 A for 100 W boards, ~3–5 A for the HL2 | PA current and bias experiments; current limiting during bring-up of new gateware | Any quality 13.8 V supply with metering | $$ |
| **Gigabit managed switch with port mirroring + Wireshark** | Port mirroring (SPAN); a gigabit NIC on the capture host | Byte-level proof of Protocol 1/2 framing, discovery replies, sequence numbers, packet pacing and watchdog behaviour — the wire half of "Verilog is the authority" | Any small managed switch with mirroring | $–$$ |
| **JTAG programmer for each FPGA family** | Vendor-supported cable | Recovery when an Ethernet update fails; on-chip logic analysis (below) | Intel USB-Blaster II for Cyclone IV (HL2) and the classic Altera boards; Digilent JTAG-HS2/HS3 or a Xilinx platform cable for the Artix-7 Saturn | $–$$ |
| **Coax and adapters** | RG-316/RG-400 for low-power and test ports, RG-213/LMR-400-class for 100 W+; N/SMA/BNC/UHF adapters; SMA torque wrench | Repeatable, low-loss connections — most "mystery" level errors are a bad adapter | — | $–$$ |
| **ESD protection** | Mat, wrist strap | Not destroying exposed FPGA and converter boards during probing | — | $ |

### Tier 2 — recommended (signal-quality and gateware DSP work)

| Item | Key specification | What it lets you prove | Examples | Cost |
|---|---|---|---|---|
| **Signal generator(s)** — two for IMD | 9 kHz–≥ 100 MHz; low phase noise (≤ −130 dBc/Hz at 10 kHz offset for reciprocal-mixing tests); clean output down to the µV range through the step attenuator | RX sensitivity/MDS, S-meter and dBm calibration, AGC behaviour, ADC-overload flag thresholds, and — with a sweep — DDC passband flatness and alias rejection | Siglent SSG3000X series; Rigol DSG800 series; a used HP 8642B (GPIB) for low close-in phase noise. Avoid synthesizer-chip eval boards (ADF435x-class) for receiver testing — their phase noise and harmonics swamp the measurement | $$–$$$$ |
| **Hybrid combiner** | 2-way 0°, ≥ 25 dB port-to-port isolation, covering 1.8–54 MHz | Two-tone receiver IMD3/IIP3 and blocking without the generators intermodulating each other | Mini-Circuits 2-way combiners — **check the low-frequency limit reaches 1.8 MHz** | $–$$ |
| **Programmable step attenuator** | 0–110/120 dB in 1 dB steps, flat to 54 MHz | Calibrated levels for MDS, S-meter linearity and dynamic-range sweeps | Mini-Circuits RCDAT-6000-90 (USB + Ethernet, 1–6000 MHz); used HP 8494/8496 pairs with an 11713 controller (GPIB) | $$–$$$ |
| **10 MHz reference (GPSDO) + distribution amplifier** | Locked 10 MHz to the radio's external reference input, the analyzer and the generators | Exercise the in-fabric 10 MHz reference loops (HL2 CL1, Saturn and Orion VCXO steering); frequency calibration; with one shared reference, frequency error is the radio's alone | Leo Bodnar GPSDOs; other GPSDO units with a 10 MHz output | $$ |
| **Oscilloscope** | ≥ 200 MHz bandwidth, 4 channels, SPI/I2C/I2S decode, deep memory (≥ 350–500 MHz to see edge quality on the 76.8 / 122.88 MHz converter clocks) | CW envelope rise/fall time and shape via the sampler (5 ms raised cosine on classic, ~4 ms ramp on HL2); PTT/TR-relay sequencing against RF; codec I2S; clock quality | Siglent SDS2000X Plus / SDS2000X HD; Rigol DHO800/DHO900 at the lower end | $$–$$$ |
| **Logic analyzer** | ≥ 8 channels, ≥ 100 MS/s, protocol decoders | Alex/filter-board SPI words, AD9866 SPI init, VersaClock and IO-board I2C, key/PTT line timing — off-chip, without perturbing the design | Saleae Logic 8 / Logic Pro; DSLogic | $–$$$ |
| **On-chip logic analysis** | Bundled with the vendor tools | Probe internal DDC/DUC samples, FIFO levels and state machines through the JTAG cable — the only way to see inside the fabric | Intel Signal Tap (Quartus); Xilinx ILA (Vivado). Check which features your free edition includes | free |
| **VNA** | 50 kHz–≥ 100 MHz, 2-port | Sweep Alex / N2ADR filter banks, verify the sampler's flatness and coupling, check dummy-load SWR and cable loss | NanoVNA-H4; LiteVNA 64; used lab VNAs for higher accuracy | $–$$$$ |
| **Shielded test enclosure** | RF-tight box with filtered DC and feed-throughs | Credible MDS/sensitivity numbers and spur hunts without local switching-supply noise | Commercial RF test box, or a well-gasketed metal enclosure | $–$$ |
| **USB audio interface** | Clean line-level I/O, 48 kHz | Controlled TX audio and two-tone injection into the mic path; repeatable audio-level tests ([`audio-test-plan.md`](audio-test-plan.md)) | Any low-noise USB interface | $–$$ |

### Tier 3 — advanced or shared lab

| Item | What it adds | Cost |
|---|---|---|
| **High-dynamic-range analyzer** (used Keysight/R&S class) | Measures SFDR and close-in spurs below what entry-level analyzers can see — needed to back numerical claims about dither or NCO-spur improvements | $$$–$$$$ |
| **Noise source and noise-figure method** | Receiver noise figure, LNA-setting validation | $$–$$$ |
| **High-power attenuator** (30–40 dB, 100–200 W) | Whole-signal analysis without a sampler; an alternative chain for PureSignal level setting | $$ |
| **Thermal camera** | PA, regulator and FPGA hot spots during long keyed tests; converter temperature drift | $$ |
| **DC current clamp / probe** | PA current against bias settings (HL2 bias registers, ANAN PA current) | $–$$ |
| **Isolation hardware** | Common-mode chokes on USB/Ethernet, isolated scope inputs or a differential probe — removes ground-loop artefacts from audio and clock measurements | $–$$$ |
| **Spare bring-up hardware** | A sacrificial HL2, a spare Raspberry Pi CM4/CM5 for Saturn, USB-serial adapters — so a failed gateware experiment does not take the main station down | $$–$$$ |

---

## 4. What to measure, and with what

| Validation | Setup | What it checks in the gateware | Tier |
|---|---|---|---|
| TX harmonics and spurs | TX chain → analyzer | Low-pass filter selection; DUC images and CIC interpolation residue; DAC clock spurs | 1 |
| TX two-tone IMD | Radio's two-tone or audio interface → TX chain → analyzer | DUC word-length and scaling (clipping in the interpolators or gain stage) | 1–2 |
| CW key-click and envelope | TX chain → scope (envelope) + analyzer (sidebands) | The envelope shape and timing implemented in fabric | 1–2 |
| Power and SWR meter scaling | Wattmeter in line, dummy load | Forward/reflected ADC scaling and conversion constants | 1 |
| PureSignal | Coupler + attenuation to the feedback input at the documented level | Feedback routing (digital DAC-sample loop-back) and the level window | 2–3 |
| RX sensitivity / MDS and S-meter | Generator → step attenuator → radio, shielded | Gain staging and dB-reference calibration | 2 |
| RX two-tone IMD3 / blocking / reciprocal mixing | Two generators → combiner → attenuator → radio | ADC/LNA headroom; NCO phase-noise and spur contribution | 2 |
| DDC passband flatness and alias rejection | Swept generator across the passband edge at each output rate | CIC-compensation FIR design and droop correction (e.g. the G2 edge-droop tables) | 2 |
| ADC overload flag | Generator level stepped up to the flag threshold | Overrange detection and its reporting path | 2 |
| Frequency accuracy and reference lock | GPSDO to radio and instruments | The in-fabric 10 MHz reference loop and frequency-word arithmetic | 2 |
| Wire protocol | Mirrored switch port + Wireshark | Framing, discovery fields, sequence numbers, pacing, watchdog timeouts | 1 |
| Internal signal paths | Signal Tap / ILA over JTAG | DDC/DUC samples, FIFO occupancy, state machines | 2 |
| Control buses | Logic analyzer on SPI/I2C | Filter-board words, converter and clock-chip initialisation | 2 |

---

## 5. Starter kits

- **HL2 developer** (5 W, lowest cost): Tier 1 sized for 5 W — small dry dummy
  load, low-power sampler, budget analyzer plus an RTL-SDR or RSP-class monitor,
  USB-Blaster II, managed switch — then a NanoVNA and a GPSDO from Tier 2.
- **G2 / ANAN developer** (100–200 W): all of Tier 1 at full power rating
  (continuous-rated load, 100 W+ sampler, 30 dB of pad after it), plus the JTAG
  cable for the board's FPGA family and a 25 A supply.
- **Shared project lab** — Tier 2 in full and selected Tier 3 items, so that DSP
  claims (dither, NCO spurs, filter rejection) are backed by measurement before
  they merge.

---

## 6. Driving the bench from Claude

Every instrument on the bench should be controllable over **LAN or USB**, so an
agent can set up a stimulus, take a measurement and record the result without a
human turning knobs. Passive parts (loads, samplers, fixed pads, coax, combiners)
cannot be driven; each gets a drivable companion that makes it observable or
switchable instead.

### How Claude reaches the gear

- **One bench host** (a small Linux PC) owns every USB instrument and sits on an
  **isolated instrument LAN** with the networked ones. Nothing on that LAN needs
  internet access.
- **One control layer**: PyVISA with the pure-Python `pyvisa-py` backend covers
  SCPI over LAN (raw socket, VXI-11, HiSLIP) and USBTMC without vendor VISA
  installs; `pyserial`, sigrok, and vendor SDKs cover the rest. Wrap each
  instrument in a small CLI or an MCP server on the bench host.
- **A cloud session cannot see USB or a private LAN.** Run Claude Code on the
  bench host itself, or serve a folder on it to a cloud session through Remote
  Control, or expose the instrument wrappers as an MCP server reachable from the
  session.

### The safety gate is not optional

Bench automation must not become a second way to key a transmitter or to put
power where it does not belong (Principles VI and XIII):

- **Transmit stays behind the automation bridge's existing TX gate.** No bench
  wrapper keys PTT, drives a key line, or switches anything into the high-power
  path. The only PTT-adjacent hardware an agent may drive is a **TX inhibit** —
  a USB relay wired so that the *energised or failed* state inhibits (the HL2 has
  an active-low TX-inhibit input).
- **Hard limits below software**: set over-voltage/over-current protection on
  the supplies and output-level limits on the generators by hand; software may
  only operate inside them.
- **RF switches and programmable attenuators sit only on the attenuated side**,
  after the fixed high-power pads. A mis-commanded switch must never be able to
  route transmit power into an analyzer.
- **Generator RF output defaults off** and is enabled per measurement.

### Per-item recommendations

Verification status: **verified** = interface and range confirmed against the
manufacturer or project page cited; **verify** = believed correct, confirm before
buying.

| BoM item | Drivable recommendation | Interface | Control path | Status |
|---|---|---|---|---|
| Dummy load | Any continuous-rated load **plus** a Pico TC-08 thermocouple logger on the load body | USB | PicoSDK / PicoLog (Linux drivers, Python wrappers) | verify |
| RF sampler | Passive; route its (padded) output with a Mini-Circuits RC-series SPnT switch (USB + Ethernet) between analyzer, monitor SDR and power sensor | USB / LAN | Mini-Circuits HTTP/SCPI-style commands | verify model and power handling |
| Fixed pads → programmable stage | Keep the high-power fixed pads; add a **Mini-Circuits RCDAT-6000-90** (0–90 dB, 0.25 dB steps, **1–6000 MHz**) after them. Not the RUDAT line, which is USB + RS-232 only | USB + Ethernet | Mini-Circuits command set | **verified** ([datasheet](https://www.minicircuits.com/pdfs/RCDAT-6000-90.pdf)) |
| Spectrum analyzer | **Siglent SSA3021X Plus / SSA3032X Plus** with tracking generator; **tinySA Ultra** as the budget unit | LAN + USBTMC; tinySA: USB serial shell | SCPI via PyVISA; tinySA `scan`/`data` serial commands | verify |
| Monitor receiver | **SDRplay RSP1B** or **Airspy HF+ Discovery** | USB (network via SoapyRemote / SpyServer) | SDRplay API 3 + SoapySDRPlay3; libairspyhf | verify |
| Wattmeter (in-line FWD/REF/SWR) | **TelePost LP-100A** on a USB-serial adapter | RS-232 → USB | Serial command set in the operations manual | verify command set ([LP-100A](https://microprecision.com/calibration/telepost-lp-100a/)) |
| Absolute RF power | **Keysight U2004A**, 9 kHz–6 GHz, −60 to +20 dBm, behind the sampler and pads. Not Mini-Circuits' USB sensors for HF: the PWR-6LGHS starts at 50 MHz, the PWR-8P-RC at 10 MHz, and the PWR-8GHS is end-of-life | USB | Documented through BenchVue; SCPI over USBTMC from Linux is unconfirmed | range **verified** ([Keysight](https://www.keysight.com/ca/en/product/U2004A/usb-power-sensor-9-khz-6-ghz.html)); Linux control **verify** |
| Bench supply, HL2 / low current | **Riden RD6024** (60 V / 24 A) or **Rigol DP832** (3 channels, 3 A each) | RD6024: USB-serial / Wi-Fi Modbus; DP832: LAN + USB SCPI | `riden-modbus` Python library; PyVISA. Recent stock Riden firmware may break Modbus — test your unit | Riden **verified** with caveat ([riden-modbus](https://pypi.org/project/riden-modbus/)); DP832 verify |
| Bench supply, 100–200 W boards | **TDK-Lambda Z+ Z20-40-LAN** (0–20 V, 0–40 A, LAN); size ≥ 45–50 A for 200 W boards | LAN | SCPI via PyVISA | **verified** ([listing](https://www.testequity.com/product/39496-1-Z20-40-LAN-L-U)) |
| Packet capture | A dedicated capture NIC on the bench host, fed by a switch with port mirroring configured once | — | `tshark` / `dumpcap` (fully scriptable) | n/a |
| JTAG, Intel/Altera (HL2, classic ANAN) | **Intel USB-Blaster II** | USB; network via `jtagd` remote JTAG | `quartus_pgm`, `quartus_stp` (Signal Tap); openFPGALoader as an open alternative | verify |
| JTAG, Xilinx (Saturn Artix-7) | **Digilent JTAG-HS3** | USB; network via Vivado `hw_server` | Vivado Tcl (program, ILA); openFPGALoader | verify |
| Signal generators (×2 for IMD) | **Siglent SSG3021X Plus** pair sharing the GPSDO reference; or a used **HP 8642B** through an NI GPIB-USB-HS adapter for lower close-in phase noise | LAN + USBTMC; GPIB → USB | SCPI via PyVISA; linux-gpib for GPIB | verify |
| Hybrid combiner | Passive; tones are switched by the generators' RF on/off | — | — | n/a |
| Step attenuator (receive chain) | **Mini-Circuits RCDAT-6000-90**, or **RCDAT-6000-110** if 110 dB range is needed | USB + Ethernet | as above | -90 **verified**; -110 range **verify** |
| GPSDO reference | **Leo Bodnar Mini GPSDO** or **LBE-1420** | USB HID | `lbgpsdo` (status: SAT/PLL lock, loss count) / `lbe-142x` CLIs | **verified** ([lbgpsdo](https://github.com/hamarituc/lbgpsdo)) |
| Oscilloscope | **Siglent SDS2000X HD** (or SDS2000X Plus) | LAN + USBTMC | SCPI via PyVISA; waveform readout | verify |
| Logic analyzer | **Saleae Logic Pro 8 / Pro 16**; **DSLogic** as the sigrok alternative | USB | Saleae Logic 2 automation API (gRPC, `logic2-automation`, port 10430; needs Xvfb on Linux); `sigrok-cli` | Saleae **verified** ([logic2-automation](https://github.com/saleae/logic2-automation)) |
| On-chip analysis | Signal Tap (Quartus) / ILA (Vivado) | via the JTAG cables | `quartus_stp` / Vivado Tcl | verify edition features |
| VNA | **LiteVNA 64** or **NanoVNA-H4**; **Copper Mountain S5048** for lab accuracy | USB serial; Copper Mountain via its software's TCP SCPI | NanoVNA serial command set; SCPI | verify |
| Shielded enclosure | Passive | — | — | n/a |
| Audio interface | Any USB class-compliant interface (e.g. Focusrite Scarlett 2i2) | USB | ALSA / PortAudio (`sounddevice`) | verify |
| High-dynamic-range analyzer | Used **Keysight N9010A/B EXA** or **R&S FPC1500** | LAN | SCPI via PyVISA | verify |
| Noise source | Passive source; switch its +28 V from a programmable supply channel for Y-factor | — | via the supply | n/a |
| Thermal camera | **PureThermal 3 + FLIR Lepton 3.5** | USB (UVC, radiometric) | libuvc / PureThermal SDK | verify |
| PA current | Use the programmable supply's current readback rather than a clamp | — | via the supply | n/a |
| TX inhibit interlock | USB relay board wired fail-safe to the radio's TX-inhibit input | USB | Relay vendor CLI / serial | verify wiring per radio |
