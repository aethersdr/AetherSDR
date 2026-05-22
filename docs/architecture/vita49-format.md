# VITA-49 Packet Format Reference

Extracted from CLAUDE.md for on-demand reference. Read this when working
on PanadapterStream, FFT/waterfall rendering, or meter data parsing.

## VITA-49 Packet Format

### Header (28 bytes)

Words 0–6 of the VITA-49 header. Key field: **PCC** in lower 16 bits of word 3.

### Packet Class Codes

| PCC | Content | Payload Format |
|------|---------|---------------|
| `0x8003` | FFT panadapter bins | uint16 big-endian, linear map to dBm |
| `0x8004` | Waterfall tiles | 36-byte sub-header + uint16 bins |
| `0x03E3` | RX audio (uncompressed) | float32 stereo, big-endian |
| `0x0123` | DAX audio (reduced BW) | int16 mono, big-endian |
| `0x8002` | Meter data | N × (uint16 meter_id, int16 raw_value), big-endian |

### FFT Bin Conversion

The radio encodes FFT bin values as **pixel Y positions** (0 = top/max_dbm,
ypixels-1 = bottom/min_dbm), NOT as 0-65535 uint16 range:

```
dBm = max_dbm - (sample / (y_pixels - 1.0)) × (max_dbm − min_dbm)
```

`min_dbm` / `max_dbm` come from `display pan` status messages (typically -135 / -40).
`y_pixels` comes from `display pan` status (must be tracked per-stream via
`PanadapterStream::setYPixels()`).

### FFT Frame Assembly

FFT data may span multiple VITA-49 packets. A 12-byte sub-header at offset 28
contains: `start_bin_index`, `num_bins`, `bin_size`, `total_bins_in_frame`,
`frame_index`. `PanadapterStream::FrameAssembler` stitches partial frames.

### Waterfall Tile Format

36-byte sub-header at offset 28:

| Offset | Type | Field |
|--------|------|-------|
| 0 | int64 | FrameLowFreq |
| 8 | int64 | BinBandwidth |
| 16 | uint32 | LineDurationMS |
| 20 | uint16 | Width |
| 22 | uint16 | Height |
| 24 | uint32 | Timecode |
| 28 | uint32 | AutoBlackLevel |
| 32 | uint16 | TotalBinsInFrame |
| 34 | uint16 | FirstBinIndex |

Payload: `Width × Height` uint16 bins (big-endian). Conversion:

```
intensity = static_cast<int16>(raw_uint16) / 128.0f
```

This yields an **arbitrary positive intensity scale** (NOT actual dBm).
Observed values: noise floor ~96–106, signal peaks ~110–115 on HF.
The waterfall colour range is calibrated to [104, 120] by default and is
**decoupled** from the FFT spectrum's dBm range.

### Audio Payload

- PCC 0x03E3: big-endian float32 stereo → byte-swap uint32, memcpy to float,
  scale to int16 for QAudioSink (24 kHz stereo)
- PCC 0x0123: big-endian int16 mono → byte-swap, duplicate to stereo

### Meter Data Payload (PCC 0x8002)

Payload is N × 4-byte pairs: `(uint16 meter_id, int16 raw_value)`, big-endian.
Value conversion depends on unit type (from FlexLib Meter.cs):

| Unit | Conversion |
|------|-----------|
| dBm, dB, dBFS, SWR | `raw / 128.0f` |
| Volts, Amps | `raw / 1024.0f` (v1.4.0.0) |
| degF, degC | `raw / 64.0f` |

### Meter Status (TCP)

Meter definitions arrive via TCP status messages with `#` as KV separator
(NOT spaces like other status objects). Format:
`S<handle>|meter 7.src=SLC#7.num=0#7.nam=LEVEL#7.unit=dBm#7.low=-150.0#7.hi=20.0`

The S-Meter is the "LEVEL" meter from source "SLC" (slice).

### Stream IDs (observed)

- `0x40000000` — panadapter FFT (same as pan object ID)
- `0x42000000` — waterfall tiles
- `0x04xxxxxx` — remote audio RX (dynamically assigned)
- `0x00000700` — meter data

