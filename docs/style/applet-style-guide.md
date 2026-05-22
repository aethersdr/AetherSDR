# AetherSDR Applet Style Guide

Design language reference for all applet panels in AetherSDR. Every applet
should follow these conventions to maintain visual consistency across the UI.

---

## Color Palette

### Core Theme

| Role | Hex | Usage |
|------|-----|-------|
| Background (app) | `#0f0f1a` | Main window, dark surfaces |
| Text (primary) | `#c8d8e8` | Standard labels, values, button text |
| Text (secondary) | `#8090a0` | Dim labels, slider row labels, section headers |
| Text (tertiary) | `#708090` | Section keywords ("STEP:", "Filter:") |
| Text (scale) | `#607080` | EQ dB scale, minor annotations |
| Text (inactive) | `#405060` | Disabled indicators, inactive state text |
| Accent | `#00b4d8` | Slider handles, active fills, pressed buttons |
| Title bar text | `#8aa8c0` | Applet title labels |

### Borders & Surfaces

| Role | Hex | Usage |
|------|-----|-------|
| Button base bg | `#1a2a3a` | Default button and combo background |
| Button hover bg | `#203040` | Hover state for buttons |
| Button alt hover | `#204060` | Hover state (variant used in P/CW) |
| Border (standard) | `#205070` | Button and combo borders |
| Border (subtle) | `#203040` | Triangle buttons, secondary borders |
| Inset background | `#0a0a18` | Value readout boxes, gauge bar fill area |
| Inset border | `#1e2e3e` | Value readout box borders, divider lines |
| Title gradient top | `#3a4a5a` | Title bar gradient stop 0 |
| Title gradient mid | `#2a3a4a` | Title bar gradient stop 0.5 |
| Title gradient bot | `#1a2a38` | Title bar gradient stop 1 |
| Title border | `#0a1a28` | Title bar bottom border |
| Groove | `#203040` | Slider groove background |

### Active/Checked Button States

| State | Background | Text | Border | Usage |
|-------|-----------|------|--------|-------|
| Green | `#006040` | `#00ff88` | `#00a060` | PROC, +ACC, MON, Sidetone, Breakin, NB/NR/ANF, EQ ON |
| Blue | `#0070c0` | `#ffffff` | `#0090e0` | DAX, Iambic, filter presets, antenna, EQ RX/TX |
| Amber | `#604000` | `#ffb800` | `#906000` | RIT, XIT, QSK |

### Gauge Fill Zones

| Zone | Color | Usage |
|------|-------|-------|
| Normal | `#00b4d8` (cyan) | Safe operating range |
| Warning | `#ddbb00` (yellow) | Approaching limit |
| Danger | `#ff4444` (red) | Over limit / clipping |
| Peak marker | `#ffffff` (white) | Peak hold indicator line |

---

## Typography

| Context | Size | Weight | Color |
|---------|------|--------|-------|
| Title bar | 10px | Bold | `#8aa8c0` |
| Button text | 10px | Bold | `#c8d8e8` |
| Slider row labels ("Delay:", "Speed:") | 11px | Normal | `#8090a0` |
| Section headers ("STEP:", "Filter:") | 11px | Normal | `#708090` |
| Value readouts (inset boxes) | 10-11px | Normal | `#c8d8e8` |
| Gauge tick labels | 9px | Normal | Zone-dependent |
| Gauge center label | 10px | Bold | `#8090a0` |
| Scale annotations (EQ dB) | 9px | Normal | `#607080` |
| Proc tick labels (NOR/DX/DX+) | 8px | Normal | `#c8d8e8` |

No explicit font family is set anywhere; all text uses the Qt system default.

---

## Components

### Title Bar

Every applet starts with a shared gradient title bar via `appletTitleBar(text)`:

- **Height:** 16px fixed
- **Background:** vertical linear gradient `#3a4a5a` -> `#2a3a4a` -> `#1a2a38`
- **Bottom border:** 1px solid `#0a1a28`
- **Label:** 10px bold `#8aa8c0`, positioned at (6, 1), transparent background

### Buttons

**Standard toggle button:**
```css
QPushButton {
    background: #1a2a3a;
    border: 1px solid #205070;
    border-radius: 3px;
    color: #c8d8e8;
    font-size: 10px;
    font-weight: bold;
    padding: 2px 4px;
}
QPushButton:hover { background: #204060; }
```

Pair with a checked-state override (`kGreenActive`, `kBlueActive`, `kAmberActive`)
depending on the button's semantic role.

**Triangle step button (TriBtn):**
- **Size:** 22x22px fixed
- **Style:** `#1a2a3a` bg, `#203040` border, 3px radius
- **Hover:** `#203040`
- **Pressed:** `#00b4d8`
- Paints a filled triangle (left or right), `#c8d8e8` normally, black when pressed

### Sliders

**Horizontal:**
```css
QSlider::groove:horizontal {
    height: 4px; background: #203040; border-radius: 2px;
}
QSlider::handle:horizontal {
    width: 10px; height: 10px; margin: -3px 0;
    background: #00b4d8; border-radius: 5px;
}
```

**Vertical (EQ bands):**
```css
QSlider::groove:vertical {
    width: 4px; background: #203040; border-radius: 2px;
}
QSlider::handle:vertical {
    height: 10px; width: 16px; margin: 0 -6px;
    background: #00b4d8; border-radius: 5px;
}
```

### Inset Value Display

Used for numeric readouts (step size, RIT/XIT, pitch, filter cuts, slider values):

```css
QLabel {
    font-size: 10px;
    background: #0a0a18;
    border: 1px solid #1e2e3e;
    border-radius: 3px;
    padding: 1px 2px;
    color: #c8d8e8;
}
```

- **Alignment:** centered
- **Width:** 36-48px (narrow values), 48-60px (wider values like frequencies)

### Combo Boxes

All combo boxes **must** use the shared style from `src/gui/ComboStyle.h`:

```cpp
#include "ComboStyle.h"

auto* combo = new QComboBox;
AetherSDR::applyComboStyle(combo);
```

This applies the standard dark theme with a painted down-arrow triangle:

```css
QComboBox {
    background: #1a2a3a;
    color: #c8d8e8;
    border: 1px solid #304050;
    padding: 2px 2px 2px 4px;
    border-radius: 2px;
}
QComboBox::drop-down {
    border: none;
    width: 14px;
}
QComboBox::down-arrow {
    image: url(<generated arrow>);
    width: 8px;
    height: 6px;
}
QComboBox QAbstractItemView {
    background: #1a2a3a;
    color: #c8d8e8;
    selection-background-color: #00b4d8;
}
```

The down-arrow is a dynamically generated 8x6px PNG triangle in `#8aa8c0`,
cached in the system temp directory. **Do not** define local `comboArrowPath()`
functions or inline combo stylesheets — always use `applyComboStyle()`.

> **Note:** Padding is intentionally compact (`2px 2px 2px 4px`) to fit text
> in narrow combo boxes (e.g., AGC mode at 52px width). If text is clipped,
> widen the combo rather than increasing global padding.

### HGauge (Horizontal Bar Gauge)

Reusable widget from `src/gui/HGauge.h`.

- **Height:** 24px fixed
- **Bar background:** `#0a0a18`
- **Bar border:** `#203040`
- **Fill:** three-zone (cyan/yellow/red) based on `yellowStart` and `redStart` thresholds
- **Reversed mode:** single red fill from right to left (used for compression)
- **Peak hold:** white vertical line marker, enabled via `setPeakValue()`
- **Tick labels:** 9px, colored by zone (cyan normal, yellow warning, red danger)
- **Center label:** 10px bold `#8090a0`

### RelayBar (Tuner Relay Indicators)

- **Height:** 18px fixed
- **Label:** 24px wide, 10px bold `#c8d8e8`
- **Bar fill:** cyan `#00b4d8`, range 0-255
- **Value text:** 28px wide, right-aligned

### MeterSmoother (Meter Ballistics)

Every meter / level-bar / GR readout drives its display value through
`MeterSmoother` (`src/gui/MeterSmoother.h`) so the whole interface reads as
one consistent instrument.

- **Attack:** 30 ms · **Release:** 180 ms · **Poll rate:** 120 Hz
  (`kMeterSmootherIntervalMs` = 8 ms)
- **Domain:** normalised `[0, 1]` — callers map physical units (dBFS,
  dB of GR, etc.) to a fraction before `setTarget()`
- **Driving timer:** `QTimer` + `QElapsedTimer` per the header's usage
  example; `tick(elapsedMs)` integrates over wall clock so timer
  jitter doesn't change the perceived ballistic

Don't roll your own envelope follower or copy ad-hoc smoothing constants
from another widget. If a meter genuinely needs different ballistics
(e.g. a slower release for a specialised GR bar), opt into different
constants via `MeterSmoother::Ballistics`.

### ClientCompKnob (Rotary Knob)

Reusable knob widget from `src/gui/ClientCompKnob.h` used throughout the
TX/RX DSP chain editors and the Aetherial Audio Channel Strip.

- **Standard size:** 76×76 px (channel-strip / editor); 38×48 px on
  applet tiles for compact mode
- **Center label:** enable via `setCenterLabelMode(true)` to render the
  numeric value inside the dial; otherwise the value renders below
- **Range:** `setRange(min, max)` + `setDefault(v)`; double-click resets
  to default
- **Format:** `setLabelFormat([](float v){ return ...; })` for
  unit-suffixed display (`"+3.0 dB"`, `"15 ms"`, etc.)

---

## Layout

### Margins & Spacing

| Context | Margins (L, T, R, B) | Spacing |
|---------|----------------------|---------|
| Outer applet layout | 0, 0, 0, 0 | 0 |
| Inner content area | 4, 2, 4, 2-6 | 2-4px |
| Slider row | — | 4px between items |

### Slider Row Pattern

Standard layout for a labeled slider with value readout:

```
[Label/Button (fixed width)] [gap] [Slider (stretch 1)] [Inset Value (fixed width)]
```

- **Left column width:** 70px (accommodates "Sidetone" button)
- **Gap:** 4px (between label and slider)
- **Value box width:** 36px
- **Row spacing:** 4px

### Button Row Pattern

Buttons in a row use stretch or fixed width depending on context:

- **Equal-width buttons:** `setSizePolicy(QSizePolicy::Ignored, QSizePolicy::Fixed)` + stretch
- **Fixed-width buttons:** `setFixedWidth(N)` or `setFixedSize(W, H)`
- **Standard height:** 22px

### Dividers

Thin horizontal lines between sections:
```css
QFrame { color: #1e2e3e; }
```
Using `QFrame::HLine` with `QFrame::Sunken`, fixed height 2px.

---

## Patterns

### Feedback Loop Prevention

All applets use one of two guard patterns when syncing UI state from the model:

1. **Boolean guard** (`m_updatingFromModel`): set true before updating widgets,
   checked in signal handlers to suppress outgoing commands.

2. **QSignalBlocker**: blocks signals on a specific widget during programmatic
   updates (preferred for combo boxes).

### Stepper Controls

For values adjusted in discrete steps (pitch, RIT/XIT, filter cuts):

```
[TriBtn left] [Inset Value Display] [TriBtn right]
```

- Step buttons are 22x22px triangle buttons
- Value display is an inset label (centered)
- Click handler reads current value from model, applies delta, sends command

### Mode-Aware Panels

Use `QStackedWidget` when a single applet slot shows different controls based
on radio state (e.g., P/CW applet switches between Phone and CW panels based
on slice mode). Connect the mode signal to `setCurrentIndex()`.

### Gauge Configurations

Common gauge setups across applets:

| Gauge | Min | Max | Yellow | Red | Reversed |
|-------|-----|-----|--------|-----|----------|
| Fwd Power (barefoot) | 0 | 200 W | 125 | 125 | No |
| Fwd Power (PGXL) | 0 | 2000 W | 1250 | 1250 | No |
| SWR | 1.0 | 3.0 | 2.5 | 2.5 | No |
| Mic Level | -40 | +10 dBFS | -10 | 0 | No |
| Compression | -25 | 0 dB | — | 1 | Yes |
| ALC | 0 | 100 | 80 | 80 | No |

---

## Applet Anatomy

A typical applet follows this vertical structure:

```
┌─────────────────────────────────┐
│  Title Bar (16px gradient)      │
├─────────────────────────────────┤
│  Gauge(s)           (if any)    │
├─────────────────────────────────┤
│  Combo row(s)       (if any)    │
├─────────────────────────────────┤
│  Slider rows                    │
│  [Label] [Slider] [Value]       │
│  [Label] [Slider] [Value]       │
├─────────────────────────────────┤
│  Toggle button rows             │
│  [Btn] [Btn] [stretch] [Btn]   │
├─────────────────────────────────┤
│  Stepper rows       (if any)    │
│  [Btn] [< value >]             │
└─────────────────────────────────┘
```

Content margins: 4px sides, 2px top, 2-6px bottom. Row spacing: 2-4px.
