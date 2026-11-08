# FLEX-1500 native backend — design proposal

**Status:** Proposal. Not approved, not scheduled. Queued behind the backend
freeze recorded on #5554 (2026-09-11: "no new radio backends until aetherd is
complete"), in the same position as #5355 and #5474. The capture program in §6
is research, not code: the maintainer cleared it, and tester recruitment, to
start during the freeze (2026-11-08).
**Family string:** `flex1500` (proposed)
**Transport:** local USB 1.1 Full Speed, through libusb
**Phase 1 scope:** receive only, `canTransmit = false`

## 1. Why

The FLEX-1500 is FlexRadio's PowerSDR-era 5 W QRP transceiver: a USB
direct-conversion radio that ships raw baseband I/Q to the host and leaves all
demodulation, filtering and display to host software. FlexRadio stopped
developing PowerSDR, and the radio has no supported client on Linux or macOS.
Owners still use it, often as the IF deck for VHF and microwave transverters,
which is the role the Service Manual describes for it.

On AetherSDR's side the radio sits on the same branch of the seam as HL2, ANAN
and RTL-SDR: raw I/Q in, engine-side DSP. That branch already exists, so the
new work is a USB transport, a small command encoder, and a capability
declaration. The hard part is that FlexRadio never published the USB
transport. This document separates what open sources settle from what only
wire captures can settle, and it does not guess at the second set.

### 1.1 Scope

In scope for phase 1:

- discovery of an attached FLEX-1500 over USB;
- one receiver and one panadapter at the radio's fixed 48 kHz I/Q rate;
- tuning, band-filter selection that follows tuning, and the preamp steps;
- read-only identity: serial number, firmware revision, region;
- read-only use of the factory RX calibration tables, if §9 decides to.

Out of scope for phase 1, by construction rather than by omission:

- any transmit path (MOX, TUNE, CW keying, the QSE, the PA filters, the
  transverter TX outputs), per §8;
- the radio's own headphone, speaker and line outputs (audio goes to the PC);
- firmware loading or EEPROM writes of any kind;
- FlexWire accessories, the external I2C bus, and the production test fixture;
- a second receiver. The radio has one QSD.

FLEX-5000 and FLEX-3000 are FireWire radios with a different, also closed,
transport. They are not covered here and could be a later family at most.

## 2. Protocol authority

`docs/agents/backends.md` requires a new family to name its open protocol
authority (Constitution Principles I and IV). For `flex1500` the authority is
the following, and nothing else.

### 2.1 Clean sources

1. **FlexRadio's GPL PowerSDR source** (© FlexRadio Systems, GPL v2 or later).
   Every file listed was confirmed as FlexRadio-authored GPL source by its
   copyright header.
   - `TAPR/OpenHPSDR-SVN`, path
     `WD5Y/PowerSDR - vs2008/HPSDR @ v1.2.2/Source/Console/hid/usbhid.cs`
     (© 2004–2010). This is the oldest copy of the `USBHID.Opcode` enum, with
     238 opcodes, and its wrappers. Its sibling `usbhidapi.cs` declares the
     host-side call surface (`Init`, `WriteOp`, `ReadOp`, `ReadEEPROM`,
     `WriteEEPROM`).
   - `M0LTE/PowerSDR` at commit `89fe7c4680` ("original source code"), the
     unmodified import of FlexRadio PowerSDR 2.7.2 (SVN 5123), © 2003–2013. Only
     this commit is used, not the repository's later history. These files carry
     no third-party modification markers:
     - `Source/Console/hid/usbhid.cs`: 246 opcodes, adding 1377–1384;
     - `Source/Console/hid/flex1500.cs`: the managed device API;
     - `Source/Console/hid/hid_eeprom.cs`: the EEPROM layout;
     - `Source/Console/hid/CodecDebug.cs`;
     - `Source/Console/console.cs` and `Source/Console/audio.cs`: call order,
       tuning arithmetic and filter tables.

   The citations below use `[GPL-SRC]` with the file and symbol. Line numbers
   are at `89fe7c4680` unless the TAPR copy is named.
2. **FlexRadio public documentation:**
   - *FLEX-1500 Software Defined Radio Service Manual*, Preliminary, Revision 0.1,
     G. Haddock KE9H, © FlexRadio Systems 2010. Also available on ManualsLib:
     <https://www.manualslib.com/manual/1392309/Flexradio-Systems-Flex-1500.html>.
   - *FLEX-1500 Owner's Manual*, v2.0, © FlexRadio Systems 2010.
   - *PowerSDR v2.7.2 Release Notes* (firmware coupling):
     <https://edge.flexradio.com/www/uploads/20200818184943/PowerSDR-v2.7.2-Release-Notes.pdf>.

   Cited as `[MANUAL]` (SM = Service Manual, OM = Owner's Manual).
3. **Public component datasheets:**
   - Analog Devices AD9951, Rev. 0 (2003);
   - Texas Instruments TAS1020B Data Manual, SLES025A;
   - Texas Instruments TLV320AIC33, SLAS480B.

   Cited as `[DATASHEET]`.
4. **Wire captures** made under the program in §6. Facts that depend on them
   are tagged `[CAPTURE-PENDING]` until a capture exists, and they are open
   questions until then, not assumptions.

FlexRadio has never published the USB transport or its driver. A community
request for it in March 2024 got no staff answer:
<https://community.flexradio.com/discussion/8029820/flex1500-usb-protocol-or-driver-source>.

### 2.2 What the clean sources do not contain

The GPL files are the host side of a call boundary. `flex1500.cs` hands every
`WriteOp(opcode, p1, p2)`, `ReadOp`, EEPROM access and audio start/stop to a
closed native library. Everything below that boundary is absent from the clean
sources:

- endpoint use;
- how one opcode and its two parameters are framed on the wire;
- the stream sample format, packet size and synchronization mode;
- start, stop and keying order at the USB level.

That gap is exactly what §6 fills. No protocol fact in this document, and no
line of a future implementation, comes from inside that closed boundary. Raw
capture files for §6 may come from any owner. Analysis, field maps and code come
only from contributors working from the §2.1 sources and the captures
themselves (Principle IV).

## 3. Hardware summary

All from the Service Manual unless tagged otherwise.

| Block | Fact | Tag |
|---|---|---|
| Host link | TAS1020B streaming USB controller; USB 1.1, Full Speed (12 Mb/s), carrying both control and streaming baseband | [MANUAL] SM p.4, p.7 |
| Controller | 8052 core; boot loader in ROM loads application code from host or EEPROM; up to 7 IN + 7 OUT endpoints plus control; all four transfer types; two DMA channels, each serving one isochronous endpoint to/from the codec port; I2S codec port; I2C master only; 6 MHz crystal | [DATASHEET] TAS1020B §1, §1.1 |
| Boot | Firmware lives in the I2C EEPROM. Without valid firmware, the controller runs from ROM and enumerates as a "TI DFU device". The blue power-button LED lights only after valid firmware has loaded and initialized | [MANUAL] SM p.7, p.15 |
| I2C | Three buses (computer, internal, external/FlexWire) behind a PCA9542A mux. 8-bit addresses: codec `0x30`, EEPROM `0xA0` (24C128), mux `0xE0`, expanders `0x40`/`0x42` (PCA9555) | [MANUAL] SM p.7, App. D |
| SPI | Outbound only. A 3.3 V bus drives the AD9951 DDS; a 5 V bus drives the TPIC6B595 relay driver on the RFPA | [MANUAL] SM p.8 |
| RFPA relays | TPIC6B595 bits 0–7: 15M, 160M, 20M, 6M, 80M, 10M, XMIT, 40M (bit 6 high = transmit) | [MANUAL] SM p.8 |
| Reference | Internal 38.4 MHz TCXO, or an external 10 MHz input doubled to 20 MHz; the DDS accepts either | [MANUAL] SM p.8, block diagram |
| LO | DDS at 2 × LO, through a 120 MHz low-pass filter into a Johnson counter that divides by 2 and produces the quadrature LO | [MANUAL] SM p.9 |
| RX front end | 11 band-pass filters, 0.49–56 MHz; preamp (ADL5531) driven directly or through a 20 dB pad; QSD mixer | [MANUAL] SM p.8, block diagram |
| Baseband | QSD I/Q 0–24 kHz; op-amps with 18 dB gain and 28 kHz low-pass into the TLV320AIC33 codec over I2S | [MANUAL] SM p.9 |
| Audio | Demodulated audio returns over USB to the codec and the radio's headphone/speaker amplifier. In TX, mic audio goes up and TX I/Q comes back down to the QSE, so the downstream path is shared | [MANUAL] SM p.9 |
| Codec | TLV320AIC33: stereo ADC/DAC, 8–96 kHz | [DATASHEET] |
| Sample rate | Fixed at 48 kHz | [MANUAL] OM, DSP buffer discussion ("sample rate fixed at 48 kHz"); [GPL-SRC] `audio.cs` `sample_rate1 = 48000` |
| Usable span | About 44 kHz maximum display width; recorded I/Q covers about 36 kHz | [MANUAL] OM, Panadapter and Wave chapters |
| RX range | "10 kHz – 60 MHz (operating – requires external, customer provided filters below 1.8 MHz to eliminate images)" | [MANUAL] OM Table 5 |
| Receiver type | Direct conversion, low IF, "software selectable from DC to 20 kHz" | [MANUAL] OM Table 5 |

The TAS1020B has two DMA channels, each serving one isochronous endpoint. That
fits one isochronous IN stream carrying RX I/Q (and mic audio in TX) and one
isochronous OUT stream carrying audio (and TX I/Q in TX). This is a
**hypothesis to test in §6**, not a fact this design relies on.

## 4. Command dictionary

### 4.1 Shape

The host API is `WriteOp(Opcode, uint p1, uint p2)` and
`ReadOp(Opcode, uint p1, uint p2, out uint result)`. A float-parameter overload
also exists. EEPROM access is separate: `ReadEEPROM(ushort offset, byte n, ...)`
and `WriteEEPROM`. [GPL-SRC] TAPR `usbhidapi.cs`; `flex1500.cs` `WriteOp`,
`ReadOp`, `ReadEEPROM`. Multi-byte identity reads are byte-swapped on the host
(`SwapBytes` in `GetTRXRev`, `GetTRXSN`, `GetSerialNum`) [GPL-SRC] `usbhid.cs`.
How those swaps relate to wire byte order is [CAPTURE-PENDING].

`flex1500.cs` also registers five device-to-host event callbacks: mic PTT,
front-panel PTT, CW dash, CW dot, and an audio-buffer exchange callback with
float I and Q in/out buffers [GPL-SRC] `flex1500.cs` `SetActiveRadio`. How each
event is delivered on the wire is [CAPTURE-PENDING]. Phase 1 consumes none of
them, but §6 captures them.

### 4.2 Opcode categories

The enum starts at 1000 (`USB_OP_CREATE_INTERFACE`). It reaches 1062 by
auto-increment, and the explicit block 1200–1384 follows. [GPL-SRC] TAPR
`usbhid.cs` `USBHID.Opcode`. The names group into these categories:

| Range | Category | Examples |
|---|---|---|
| 1000–1062 | Generic controller and evaluation-board ops, shared with FlexRadio's other USB work | `NOOP`, `I2C_WRITE_VALUE`, `I2C_WRITE_2_VALUE`, `I2C_READ_VALUE`, `SET_SAMPLE_RATE`, `WRITE_SPI`, `REBOOT`, `SET_DDS_FREQ` |
| 1200–1221 | Identity and presence | `GET_FIRMWARE_REV`, `GET_TRX_REV`/`SN`, `GET_PA_REV`/`SN`, `INITIALIZE`, `GET_SERIAL_NUM`, `GET_MODEL` |
| 1222–1243, 1308–1315, 1382–1383 | Register, EEPROM and codec access | `READ/WRITE_TRX_EEPROM_UINT8/16/32`, `READ/WRITE_CODEC_REG`, `READ/WRITE_TRX_DDS_REG`, `READ_EEPROM`, `WRITE_EEPROM` |
| 1244–1265, 1334, 1347–1349 | Frequency and filters | `SET_RX1_FREQ`, `SET_RX1_FREQ_TW`, `SET_TX_FREQ_TW`, `SET_RX1_FILTER`, `SET_PA_FILTER`, `BYPASS_RX1_FILTER` |
| 1247, 1329, 1341 | Front end | `SET_TRX_PREAMP`, `SET_RX_ATTN` |
| 1248–1255 | BITE and test | `SET_TEST`, `SET_GEN`, `SET_SIG`, `SET_IMPULSE`, `SET_QSD`, `SET_QSE` |
| 1266, 1276, 1292, 1296, 1332–1333 | Keying | `READ_PTT`, `SET_TR`, `SET_MOX`, `SET_FULL_DUPLEX`, `SET_IAMBIC`, `SET_BREAK_IN` |
| 1269–1302, 1322–1327 | Antennas, transverter and amplifier sequencing | `SET_RX1_ANT`, `SET_TX_ANT`, `SET_RCA_TX1`, `SET_XVTR`, `SET_AMP_TX1`, `SET_AMP_TX1_DELAY*` |
| 1294–1295, 1338–1339 | ATU | `ATU_SEND_CMD`, `ENABLE_ATU` |
| 1304–1305, 1342–1343 | FlexWire external bus | `FLEXWIRE_WRITE_2_VALUE` |
| 1350, 1355 | Status and region | `GET_REGION`, `GET_STATUS` |
| 1356–1374 | FLEX-5000 VU module (not FLEX-1500 hardware) | `SET_VU_*` |
| 1375–1384 | Mic and audio routing (1377–1384 first appear in the 2.7.2 copy) | `SET_MIC_SEL`, `SET_TX_GAIN`, `SET_SPK_ON`, `SET_LINE_OUT_GAIN`, `SET_MON`, `SET_MON_GAIN` |

The enum is shared with the FLEX-5000/3000 code, so a name in it does not prove
that the FLEX-1500 firmware implements it. The ops the FLEX-1500 actually uses
are the ones its own call sites exercise (§4.3). Any other opcode is unverified
until a capture shows it.

### 4.3 The phase-1 opcode set

Phase 1 uses only these. Each is taken from a FlexRadio wrapper or call site,
not from the bare enum.

| Opcode | Value | Use | Source |
|---|---|---|---|
| `USB_OP_GET_FIRMWARE_REV` | 1200 | Liveness and version gate. A result of 0 means a communication failure | [GPL-SRC] `usbhid.cs` `GetFirmwareRev`; `console.cs:9140` |
| `USB_OP_INITIALIZE` | 1219 | Device init after the version check | [GPL-SRC] `usbhid.cs` `Initialize`; `console.cs:9175` |
| `USB_OP_GET_SERIAL_NUM` | 1220 | Identity. Formatted `NNNN-NNNN` from bytes 0, 1 and a 16-bit high half | [GPL-SRC] `usbhid.cs` `GetSerialNum`; `hid_eeprom.cs` `SerialToString` |
| `USB_OP_GET_TRX_REV`, `GET_TRX_SN`, `GET_PA_REV`, `GET_PA_SN` | 1202, 1203, 1205, 1206 | Board identity for diagnostics | [GPL-SRC] `hid_eeprom.cs` `Init` |
| `USB_OP_SET_TRX_PREAMP` | 1247 | Preamp step, `p1` = `FLEX1500PreampMode` | [GPL-SRC] `usbhid.cs` `SetPreamp`; `console.cs` enum `FLEX1500PreampMode` |
| `USB_OP_SET_RX1_FILTER` | 1257 | RX band-pass filter index 0–11 | [GPL-SRC] `usbhid.cs` `SetRXFilter`; `console.cs` `Set1500Filters` |
| `USB_OP_SET_RX1_FREQ_TW` | 1347 | DDS tuning word, `p1` = FTW | [GPL-SRC] `usbhid.cs` `SetFreqTW`; `console.cs` `UpdateRX1DDSFreq` |
| `USB_OP_GET_REGION` | 1350 | Region code. Values ≥ `FRSRegion.LAST` are invalid | [GPL-SRC] `usbhid.cs` `GetRegion` |
| `USB_OP_GET_STATUS` | 1355 | Nonzero = "extended" status | [GPL-SRC] `usbhid.cs` `GetStatus` |
| EEPROM read (library call, not a `WriteOp`) | — | Read-only calibration and identity (§5.3) | [GPL-SRC] `hid_eeprom.cs` |

Preamp values [GPL-SRC] `console.cs` `FLEX1500PreampMode`: `MINUS_10 = 0`,
`ZERO = 1`, `PLUS_10 = 2`, `PLUS_20 = 3`, `PLUS_30 = 4`. These are shown in the
UI as −10, 0, +10, +20 and +30 (`console.cs:23558`). How the five steps map
onto the manual's two physical states, preamp and 20 dB pad, is an open
question (§11).

RX filter index by tuned frequency [GPL-SRC] `console.cs:14644`
`Set1500Filters`:

| Index | LO below (MHz) | PowerSDR comment |
|---|---|---|
| 0 | 0.48 | bypass |
| 11 | 0.88 | AM 1 |
| 10 | 1.6 | AM 2 |
| 9 | 2.3 | 160 m |
| 8 | 3.5 | 80 m |
| 7 | 5.2 | — |
| 6 | 7.7 | 60/40 m |
| 5 | 11.4 | 30 m |
| 4 | 17.0 | 20 m |
| 3 | 25.3 | 15/12 m |
| 2 | 37.6 | 10 m |
| 1 | 56.0 | 6 m |

These edges match the Service Manual's 11-band table (Band 1 = 37.6–56.0 MHz
… Band 11 = 0.49–0.88 MHz) [MANUAL] SM band-pass schematic. Above 56 MHz
`Set1500Filters` leaves the index at 0 (bypass). PowerSDR applies the filter
from the tuning word actually sent (`TW2Freq(tw)`), and only when the index
changes (`HIDTRXFilter` setter, `console.cs:21145`).

### 4.4 The phase-1 denylist

Everything not in §4.3 is refused at the encoder. The opcodes most important to
refuse, because they key, route RF outward, or write persistent state:

- **Keying:** `SET_TR` (1276), `SET_MOX` (1292), `SET_FULL_DUPLEX`,
  `SET_IAMBIC`, `SET_BREAK_IN`. Note that PowerSDR's `SetMOX` wrapper emits
  `USB_OP_SET_TR`, not `USB_OP_SET_MOX` [GPL-SRC] TAPR `usbhid.cs` `SetMOX`.
  Both are denied.
- **TX signal path:** `SET_QSE`, `SET_GEN`, `SET_TEST`, `SET_SIG`,
  `SET_IMPULSE`, `SET_TX_FREQ*`, `SET_TX_FILTER`, `SET_PA_FILTER`,
  `SET_TX_ANT`, `SET_RCA_TX*`, `SET_AMP_TX*`, `SET_TX_GAIN`, `SET_PA_BIAS`,
  `SET_TX_DAC`.
- **Accessories:** `ATU_*`, `ENABLE_ATU`, `FLEXWIRE_*`, `SET_XV*`.
- **Raw buses and persistence:** every `I2C_*`, `WRITE_SPI`, `WRITE_GPIO*`,
  `WRITE_*_REG`, `WRITE_*EEPROM*`, `SET_EEPROM_WC`, `REBOOT`, and the EEPROM
  write call.

## 5. Tuning, calibration and the receive geometry

### 5.1 Tuning-word derivation

Three sources, each tagged:

1. [DATASHEET] AD9951: `fO = FTW × fS / 2^32`, for `0 ≤ FTW ≤ 2^31`. The
   32-bit FTW0 register is at serial address `0x04`, and an I/O UPDATE makes a
   new word active. The REFCLK multiplier is CFR2<7:3>: values 4–20 multiply
   by that integer, and other values bypass it. SYSCLK may not exceed 400 MHz.
   With the multiplier at 20×, REFCLK must be 4–20 MHz.
2. [MANUAL] The DDS runs at 2 × LO and a Johnson counter divides by 2. The
   reference is 38.4 MHz internal, or 20 MHz from the doubled external 10 MHz.
3. [GPL-SRC] `console.cs:24231` and `:24299`: for `Model.FLEX1500`,
   `tw = 0xFFFFFFFF × (2 × f_MHz) / hid_corrected_dds_clock`, with
   `hid_corrected_dds_clock = 384.0`, or `400.0` when `FLEX1500Xref` is set,
   plus a user clock-correction term.

These agree with one another:

```
f_DDS = 2 × f_LO
FTW   = round(2^32 × f_DDS / f_SYSCLK) = round(2^33 × f_LO / f_SYSCLK)
f_SYSCLK = 384 MHz  (38.4 MHz × 10, internal TCXO)
f_SYSCLK = 400 MHz  (20 MHz × 20, external 10 MHz doubled)
LO step  = f_SYSCLK / 2^33 ≈ 0.0447 Hz (384 MHz) / 0.0466 Hz (400 MHz)
LO max   = f_SYSCLK / 4 (FTW ≤ 2^31) = 96 / 100 MHz, above the 60 MHz spec
```

The implied multipliers, 10× and 20×, are both legal AD9951 values, and 20 MHz
is the maximum REFCLK at 20×. As a cross-check, the production-test path writes
`0x6AAAAAA` with the comment "set gen to 10MHz". For a direct DDS output,
2^32 × 10 / 384 = 0x6AAAAAA.AA, so the comment is consistent with a 384 MHz
SYSCLK [GPL-SRC] `flex1500.cs` `Init`.

Two notes on the implementation:

- PowerSDR divides by `0xFFFFFFFF` rather than 2^32. The difference is
  2.3 × 10⁻¹⁰, far below the ±2.5 ppm reference stability [MANUAL] OM
  Table 5. The backend uses the datasheet's 2^32 and says so in a comment.
- PowerSDR's spur reduction sends `tw & 0xFFFF0000` and moves the remainder
  into a DSP oscillator offset (`console.cs` `FWCDDSFreq`). That parks the DDS
  on a coarse grid of SYSCLK/2^16 (`spur_tune_width`, `console.cs:14330`), and
  PowerSDR only does it when its SR checkbox is on. In AetherSDR the pan center
  and the slice are decoupled already (HERMES §6 gaps 8 and 12), so the
  backend can choose its own LO grid. It does not need to copy this scheme.
  Whether it reduces spurs on real hardware is a measurement question.

**Open: the REFCLK multiplier.** Firmware programs CFR2 over the internal SPI
bus, so no USB capture shows it. The host constants 384 and 400 are FlexRadio's
GPL, and the manual's reference frequencies make 10× and 20× the only
consistent multipliers. Verifying the multiplier still takes an RF
measurement: capture scenario 4 (§6) tunes a known carrier and checks that it
lands in the predicted FFT bin. Until that passes, the constants are
"GPL-sourced, hardware-unverified". The external-reference mode needs a
separate check, and it needs a per-radio setting because nothing reports which
reference is fitted (§11).

### 5.2 Receive geometry

- The I/Q rate is a fixed 48 kHz [MANUAL] OM; [GPL-SRC]. The panadapter span
  is the sample rate, so there is one span.
- PowerSDR's first-run default for the FLEX-1500 is a 3.8 kHz low IF
  (`if_freq = 0.003800`) with spur reduction off [GPL-SRC] `console.cs:7401`.
  It enables DC blocking on both receive DSP channels [GPL-SRC]
  `console.cs:23582`. So the slice frequency is not the LO, and the QSD
  leaves a DC artifact at the LO that has to stay out of the passband.
- PowerSDR's minimum frequency is `max(if_freq, 1 µHz)` and its maximum is
  54.0 MHz [GPL-SRC] `console.cs:23542`. The Owner's Manual says 10 kHz–60 MHz,
  and the filter table ends at 56 MHz. Phase 1 declares the conservative
  PowerSDR bound until §11 resolves the difference.
- Spectral orientation (whether a carrier above the LO lands at +f or −f) is
  [CAPTURE-PENDING]. HERMES §6 gap 14 and §16 are the reason this is measured
  against a known carrier on day one and set by a single swap flag, never
  inferred from the panadapter alone.

### 5.3 EEPROM calibration (read-only)

[GPL-SRC] `hid_eeprom.cs`, offsets in the radio EEPROM address space as the
library call takes them:

| Offset | Content |
|---|---|
| `0x1820`, 8 bytes | Last calibration date/time |
| `0x3000`–`0x303F` | RX level table: 11 bands (160–6 m) × 2 × int16, value / 10 = dB; `0x302F` checksum. All `0xFF` = uncalibrated |
| `0x3050`, `0x3080` | RX image gain and phase: 11 floats each; checksums `0x307F` and `0x30AF`. Values outside ±500 are rejected |
| `0x30B0`, `0x30E0` | TX image gain and phase (TX, not read in phase 1) |

Reading the RX level table would let `panAmplitude` claim a calibrated dBm
axis, and the image tables could seed WDSP's I/Q balance. Whether phase 1 reads
them at all is a maintainer decision (§9). Either way the backend never writes
them, and every value read goes through the range checks above (Principle VII).

## 6. Transport — OPEN, to be filled from captures

Nothing in this section is known yet. It is the deliverable of the capture
program.

### 6.1 Setup

- A Windows PC running FlexRadio PowerSDR v2.7.2 with FlexRadio's own driver,
  USBPcap and Wireshark.
- A FLEX-1500, and a 50 Ω dummy load for every TX scenario. TX scenarios are
  captured so phase 1 knows what it must never emit. Phase 1 does not implement
  them.
- A known RF source for scenario 4: a signal generator, or a second radio at
  minimum level into a dummy load near the antenna port.

The maintainer has no FLEX-1500, so a tester must be recruited. Any owner can
supply raw `.pcapng` files. The analysis is done by a contributor working only
from the §2.1 sources and the captures themselves (§2.2).

### 6.2 Scenarios

One file per scenario, with wall-clock timestamps noted for each operator
action:

1. Device enumeration and descriptors (plug in with PowerSDR closed)
2. PowerSDR launch
3. Power on (Start)
4. RX on one frequency for 10 s, with a known carrier at a noted offset
5. Tuning across a band-filter edge (for example 7.6 → 7.8 MHz)
6. Each preamp step, −10 to +30
7. Band changes covering all 12 RX filter indices
8. TUNE carrier into the dummy load at low drive
9. MOX
10. CW key, if a key is available
11. Power off
12. Application exit
13. USB unplug during RX
14. Optional: the DFU state, captured if a radio with blank or corrupt EEPROM
    firmware is available, for discovery only

### 6.3 Analysis deliverables

Each is a short written finding with the capture frame numbers it rests on:

1. **Descriptor tree:** VID/PID, configurations, interfaces, alternate
   settings, endpoints (address, type, max packet size, interval), class codes.
   Also the DFU-state identity, if scenario 14 exists.
2. **Endpoint roles:** which endpoint carries control ops, which carries RX
   I/Q, which carries downstream audio, and where PTT, dot and dash events
   arrive.
3. **Op framing:** how one `WriteOp(opcode, p1, p2)` and one `ReadOp` appear
   on the wire (transfer type, request fields or payload layout, byte order,
   reply shape). Each phase-1 opcode in §4.3 must be identified in a capture,
   not inferred.
4. **EEPROM read framing.**
5. **Stream format:** sample width, endianness, channel interleave, frame and
   packet size, isochronous synchronization mode, and the effective rate
   measured over scenario 4.
6. **Spectral orientation:** where the known carrier lands relative to the LO,
   which fixes the I/Q swap flag.
7. **Ordering and timing:** start order (init, tune, filter, preamp, stream
   start), stop order, the gap between ops, and whether the device needs an
   OUT stream running for the IN stream to flow.
8. **TX observations, for the denylist only:** which ops and stream changes
   accompany TUNE, MOX and CW, so the phase-1 encoder and tests can assert that
   none of them is ever emitted.
9. **Unplug behavior:** what libusb would observe (scenario 13), which sets the
   teardown requirements.

When these land, this section is rewritten as a normative transport
specification and every `[CAPTURE-PENDING]` tag becomes `[CAPTURE]` with a file
and frame reference. Captures themselves stay out of the repo unless the tester
agrees to publish them, because they may contain host identifiers.

## 7. Backend architecture in AetherSDR

### 7.1 Family, factory and build gate

- **Family `flex1500`.** It cannot be `flex`, because
  `RadioModel::makeBackend` falls through to `FlexBackend` for any unmatched
  family. The new branch goes before that fallthrough, with the same
  case-insensitive compare and an `#ifdef AETHER_BACKEND_FLEX1500` arm that
  returns `nullptr` with a warning when the backend is compiled out, as `rtl`
  does.
- **CMake.** `option(ENABLE_FLEX1500 ... ON)` plus
  `pkg_check_modules(LIBUSB IMPORTED_TARGET libusb-1.0)`. When both hold, define
  `AETHER_BACKEND_FLEX1500` on `aethercore`. This sits beside the existing
  `hidapi` and `librtlsdr` blocks. The RTL Windows bundle already ships
  `libusb-1.0.dll`, so the two features can share it. Using libusb directly is a
  new direct dependency even though it is already in the tree transitively, so
  the GOVERNANCE dependency rule applies.
- **Discovery.** `Flex1500Enumerator` is compiled unconditionally and stubbed
  internally when the backend is off, as `RtlSdrDiscovery.cpp` is. It polls
  libusb on a worker thread and emits `RadioInfo` with `family = "flex1500"`,
  the serial from `GET_SERIAL_NUM`, and the firmware revision. VID/PID are
  [CAPTURE-PENDING] (deliverable 1), and no value is written until a capture
  shows it. A device in the DFU state is reported as "no firmware" and is not
  connectable. AetherSDR never loads firmware.
- **Firmware gate.** PowerSDR 2.7.2 accepts revisions `0x0005030A` through
  `0x0005FFFF` [GPL-SRC] `console.cs:9065`. The backend refuses to connect
  outside that range with a clear message. It never offers to update
  firmware.
- **Linux permissions.** A udev rule is needed for non-root access. It ships
  with the VID/PID once those are captured.

### 7.2 Components

```
Flex1500Backend : IRadioBackend          (owner's thread; validates, translates verbs)
├── Flex1500Worker : QThread              (owns the libusb context + device handle,
│                                          runs the event loop, async transfers)
│     ├── Flex1500Link  (interface)       (the mockable seam, §10)
│     │     └── LibusbFlex1500Link        (real transport, filled from §6)
│     └── Flex1500OpEncoder               (allowlist; §4.3 only)
└── Flex1500RxDsp                         (48 kHz I/Q → WDSP RXA + FFT)
```

### 7.3 Threading contract

Each rule is the corresponding one in `IRadioBackend.h`:

1. `Flex1500Backend` lives on `RadioModel`'s thread and is never moved.
2. Every seam signal (`spectrumFrameReady`, `audioFrameReady`, `meterUpdate`,
   `linkStatsUpdated`, ...) is emitted on that thread. The worker hops back with
   `QMetaObject::invokeMethod(this, ..., Qt::QueuedConnection)`. A libusb
   callback never emits.
3. The worker and the device handle are private. `healthSnapshot()` and
   `linkStats()` answer from cache.
4. Phase 1 adds no new seam payload types and no seam signals, so neither the
   metatype registry nor `tests/SeamSignalProbeTable.inc` changes. If one is
   needed, it goes through `tools/gen_seam_probe_table.py`.
5. `disconnectRadio()` stops the stream, cancels in-flight transfers, joins the
   worker within a bounded wait, and emits `disconnected()` exactly once. If a
   broken USB stack does not complete cancellation in time, the worker is handed
   to a self-deleting reaper, as `RtlSdrBackend` does, rather than destroying a
   running `QThread` or closing a handle still in use.
6. Frames the worker queued before stopping are dropped by the backend's own
   connected flag. Nothing is emitted after `disconnected()`.

Control ops (tune, filter, preamp) are posted to the worker as latest-value
requests. The worker coalesces them and orders them as PowerSDR does
(`UpdateRX1DDSFreq`: tuning word, then the filter for that word). A tune burst
from a knob therefore becomes a bounded stream of ops, and the GUI thread never
blocks on USB.

### 7.4 Receive DSP route

- WDSP RXA at `dsp_rate = 48000`. HERMES §2 is the reason this is pinned, and
  the radio is natively 48 kHz, so no decimation stage is needed.
- Use `RXASetPassband` for the filter (HERMES §6 gap 13), the measured I/Q
  orientation flag (gap 14), DC blocking on, and the default low-IF offset so
  the QSD's DC artifact sits outside the passband (§5.2).
- Spectrum: a host FFT over the 48 kHz I/Q goes to `spectrumFrameReady`, then
  `RadioModel::onBackendSpectrumFrame`, then `panFeed`. It is not drawn a
  second time at the seam.
- Audio: `audioFrameReady` reaches `AudioEngine` through the
  `RadioModel::backendAudioFrameReady` relay, the HL2 route. Not sim's direct
  connect, and the two gates in `backends.md` stay apart.
- Link health: USB transfer errors, short packets and sample-count drift against
  48 kHz are counted into `linkStats()`. A starved stream is counted as a gap
  and never silently stretched.

Stream parsing is a boundary (Principle VII). Every packet is checked for
length and alignment against the captured format before any sample reaches
DSP. Malformed packets are counted and dropped, and an implausible packet size
is a protocol error that triggers a bounded reconnect, not an unbounded
allocation.

### 7.5 Capabilities (phase 1)

| Field | Value | Basis |
|---|---|---|
| `family` / `model` / `manufacturer` | `"flex1500"` / `"FLEX-1500"` / `"FlexRadio Systems"` | — |
| `canTransmit` / `txPowerMaxWatts` / `hostModulates` | `false` / `0` / `false` | §8; Principle VI |
| `maxSlices` / `maxPanadapters` / `canCreateSlices` | `1` / `1` / `false` | one QSD [MANUAL] |
| `sampleRatesHz` | `{48000}` | [MANUAL] OM; [GPL-SRC] |
| `panSpanModel` | `followsSampleRate = true`, `radioWide = true` | the span is the fixed I/Q rate |
| `tuningMinHz` / `tuningMaxHz` | PowerSDR's bound, 54.0 MHz max, pending §11 | [GPL-SRC] |
| `declaredBandRanges` | The 12 RX filter segments of §4.3, for display only | [GPL-SRC] + [MANUAL] |
| `panAmplitude` | `calibratedDbm = false` unless §9 adopts the EEPROM RX level table | §5.3 |
| `hasRadioSideDsp` / `radioOwnsDbmScale` | `false` / `false` | host DSP |
| `persistsMemories` / `hasMultiClientSessions` | `false` / `false` | — |
| `clientSettingsDomains` | Tuning, Passband, RfGain (preamp step) | the radio stores no operating state |
| `extensionNamespaces` | `["flex1500"]`: preamp step, external-reference flag, DDS clock correction | — |

Receive filters, modes and AGC reuse the existing host-DSP receive records
(`receiveFilterControl`, `receiveModeControl`, `receiveDspControl`) with the
bounds the WDSP chain actually honors. Any new capability lands as a
per-feature `std::optional` record, because the capability-boolean freeze in
`tools/check_capability_records.py` forbids new bools. The values go into
`radio-capabilities-map.md` in a FLEX-1500 profile section, as RTL has.

### 7.6 Persistence

The radio stores no operating state that AetherSDR can recall, so Principle III
does not move anything onto it. Tuning, passband and the preamp step go in the
family's `OperatingState` through `clientSettingsDomains`. The
external-reference flag and clock correction are a per-radio feature object
keyed by serial (Principle V). Nothing becomes a flat `AppSettings` key.

## 8. Transmit — a later phase, gated

Phase 1 has no TX path at all, and the encoder enforces that:

- `Flex1500OpEncoder` is an allowlist containing exactly the §4.3 set, and
  every other opcode is refused at the seam. That makes Principle VI hold by
  construction: no keying opcode can be expressed, whatever the model layer
  asks for.
- `capabilities().canTransmit = false`, so `RadioModel`'s existing TX gates
  (`refuseKeyOnTransmitIncapableBackend` and the receive-only checks) refuse
  every keying intent before it reaches the backend. The backend's own
  `setKeying`, `setTune`, `setCwKeying` and `setAtu` are no-ops that log.
- The downstream isochronous path is shared between RX audio and TX I/Q
  (§3). If §6 shows the device needs an OUT stream for the IN stream to flow,
  phase 1 sends only silence and never asserts `SET_TR` or `SET_QSE`. A test
  asserts that the OUT buffer is all zeros whenever it exists.

A TX phase may be proposed only after both of these:

1. the step-4 `TxCoordinator` contract
   (`docs/aetherd-stage4-tx-coordinator.md`) is in place, because
   `backends.md` says no protocol TX method is advertised before it;
2. captures 8–10 have established the keying ordering, and a dummy-load bench
   test shows that keying stops on every failure path: USB unplug, app crash,
   and worker stall. The radio's own behavior when the host disappears
   mid-transmit is a capture-12/13 finding, and a TX phase must not ship
   without it.

That phase would add `SET_TR`, the TX tuning word, the PA filter table from
`Set1500Filters` (TX side), QSE enable and TX I/Q on the downstream stream,
each behind the coordinator. It would be its own RFC.

## 9. HL2 scaffolding (#5554 §2.5)

#5554 §2.5 records that ANAN copied about 600 lines of HL2 host-DSP scaffolding
(spectrum framing, DSP-chain setup, sample-rate selection). The backend freeze
demoted extracting it, on the reasoning that no third host-DSP family was
coming. Landing `flex1500` would make this the third copy, and that would
re-promote §2.5. The choice is architectural and the maintainer's to make.
Three options:

- **A. Extract first.** Do the bounded §2.5 extraction (shared host-DSP
  spectrum framing and WDSP channel setup, family policy left in place, hl2 and
  anan tests unchanged), then build `flex1500` on it. This is the cleanest
  result and costs the most before any FLEX-1500 code exists.
- **B. Minimal family-local glue.** Write `Flex1500RxDsp` from scratch,
  deliberately small: one receiver, one fixed rate, no rate table, no
  multi-DDC index space. Nothing is copied from `hl2/` or `anan/`, and there are
  no "mirrors Hl2…" comments. Shared WDSP wrappers that already live outside
  those directories are used as they are. This adds a third implementation, but
  not a third copy, and it leaves §2.5 to be done on its own schedule.
- **C. Adopt the RTL prepared-receiver registry**
  (`docs/architecture/rtl-receiver-registry.md`). It is engine-shaped rather
  than HL2-shaped. However, the #5924 ruling records that a second backend
  adopting those hooks must justify that separately, and the registry is built
  for multi-receiver ownership that the FLEX-1500 does not need.

**Decision: B** (maintainer, 2026-11-08).
The FLEX-1500 is the simplest host-DSP radio the project would support: one
receiver at a fixed rate. A from-scratch receiver of that size is smaller than
the slice of HL2 that would be shared, so it does not raise the cost of a later
extraction. Under A, `flex1500` becomes the first consumer of the extracted
layer, which is a useful test of whether the extraction is general.

The other §2.5 and #5554 rules hold whatever is chosen. No `family ==
"flex1500"` branches above the seam. No raw wire text above the seam (the
command-plane freeze: a new file stays at zero). No `dynamic_cast` to the
concrete backend. No capability without a verb behind it.

## 10. Testing

No FLEX-1500 is in CI, and none is needed for the tests below. All of them run
against `Flex1500Link`, an injected interface with a fake implementation, the
same seam pattern `RtlSdrWorker::Device` uses.

- **Encoder allowlist:** every §4.3 op encodes, and every other enum value is
  refused. Pinned over the full enum so a later addition cannot slip in a
  keying op.
- **Tuning word:** FTW at a table of frequencies against the closed form in
  §5.1 for both clocks, the band edges, and the 2^31 bound. The
  `0x6AAAAAA` ↔ 10 MHz direct-output cross-check.
- **Filter selection:** the index at both sides of every edge in §4.3, and that
  an unchanged index emits no op.
- **Op ordering:** a tune emits the tuning word, then the filter if it changed.
  A burst of knob tunes is coalesced into a bounded op count.
- **Stream boundary (Principle VII):** truncated, oversized, misaligned and
  zero-length packets are counted and dropped without a crash or allocation
  growth. A fuzz corpus once the format is captured.
- **RX DSP:** synthetic 48 kHz I/Q with a tone at a known offset is
  demodulated on the correct sideband, for both values of the orientation flag.
- **Contract:** `backend_seam_affinity_test` coverage for `flex1500` (rule 1
  plus a cold construct and teardown). A worker-stall teardown test pins rule 5
  with a fake device whose cancel never completes. A post-`disconnected()`
  emission test pins rule 6.
- **Fail-closed TX:** every keying intent through `RadioModel` is refused
  before the backend. The fake device records zero keying ops and an all-zero
  OUT buffer.
- **EEPROM parsing**, if adopted: all-`0xFF` tables, out-of-range floats and a
  bad checksum all leave the tables unused.

Tests register in `tests.cmake` as usual. Hardware bring-up follows the HERMES
§16.6 checklist on the tester's radio: the known-carrier orientation check
first, then the tuning-accuracy check against §5.1.

## 11. Risks and open questions

### 11.1 Open protocol questions

1. **The whole transport** (§6): VID/PID, endpoints, op framing, stream format,
   byte order. [CAPTURE-PENDING]
2. **Does the IN stream need a running OUT stream?** This decides whether
   phase 1 has a downstream path at all, and it is a Principle VI item (§8).
   [CAPTURE-PENDING]
3. **Is the REFCLK multiplier really 10× (internal) and 20× (external)?** The
   GPL constants and the manual agree, but only an RF measurement can confirm
   them (§5.1).
4. **Which reference is in use?** No known op reports whether the external
   10 MHz is fitted or selected, so for now it is a per-radio setting.
5. **What is the top of the tuning range: 54, 56 or 60 MHz?** PowerSDR says 54,
   the filter table says 56, the Owner's Manual says 60. Phase 1 uses 54.
6. **How do the five preamp steps map onto the preamp and 20 dB pad** the
   manual describes, and what are the real gain values?
7. **How should the 0.48 MHz bypass region be treated?** The Owner's Manual
   requires external filtering below 1.8 MHz. Should tuning there carry a
   warning?
8. **Spectral orientation and I/Q balance** on real hardware.
   [CAPTURE-PENDING]
9. **What does `GET_STATUS` "extended" mean on a FLEX-1500,** and does it
   change anything the backend should declare?
10. **How does the radio behave when the host disappears** (unplug, crash)? It
    matters most for TX, but phase 1's reconnect path needs it too.
    [CAPTURE-PENDING]
11. **TAS1020B DMA hypothesis:** one isochronous IN and one isochronous OUT
    (§3). [CAPTURE-PENDING]

### 11.2 Maintainer decisions

Decided 2026-11-08: the backend stays queued behind the freeze, the capture
program (§6) and tester recruitment start now, and scaffolding is option B
(§9). Still open:

1. **Allowlist encoder** as the Principle VI mechanism for phase 1 (§8). The
   recommendation is yes.
2. **EEPROM calibration:** should phase 1 read the RX level and image tables
   (§5.3) for a calibrated dBm axis and I/Q balance, or ship uncalibrated?
3. **Radio-side audio:** should PowerSDR's routing of RX audio back to the
   radio's headphone/speaker ever be supported? It shares the TX downstream
   path, so it stays out of phase 1 regardless.
4. **libusb as a new direct dependency** under the GOVERNANCE dependency rule,
   and whether Windows and AppImage builds carry it.
5. **Experimental landing:** is a backend verified on one tester's radio
   landable as experimental, as ANAN and RTL were?

### 11.3 Risks

- **No hardware in the project.** Everything after this document depends on
  recruiting a tester, and phase 1 cannot be certified without their radio.
- **Firmware coupling.** PowerSDR 2.7.2 accepts only firmware 0.5.3.10 through
  0.5.255.255 (§7.1, formatted as `RevToString` does). A radio flashed by other software may sit outside that range. The
  backend refuses rather than guesses.
- **Isochronous USB on macOS and Windows.** libusb's isochronous support and
  driver binding differ by platform. Windows in particular needs a
  WinUSB/libusbK binding that conflicts with FlexRadio's own driver. Linux is
  the likely first platform, and cross-platform parity is a stated project
  goal, so this needs a plan before landing, not after.
- **Provenance.** The USB transport is closed, so clean-room discipline rests
  on the §2.1 source list and the capture program's provenance rules (§2.2,
  §6.1).
