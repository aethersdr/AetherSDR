#!/usr/bin/env node
// Programmatic icon generator for the AetherSDR Ulanzi plugin.
//
// Every action in the plugin gets an SVG icon (144×144 viewBox, rounded-rect
// background in a category colour, bold white label centred, optional small
// sub-glyph below — e.g. ▲ / ▼ for paired up/down actions).  Palette matches
// the AetherSDR dark theme we maintain upstream so the button faces feel
// like a continuation of the desktop app.
//
// Re-run any time the ICONS list below changes — the script is idempotent
// and just overwrites the SVGs in com.g0jkn.aethersdr.ulanziPlugin/assets/icons/.
//
//   node scripts/generate-icons.js
//
// No npm deps; pure-Node fs + template literals.

import { writeFileSync, mkdirSync } from 'node:fs';
import { dirname, join } from 'node:path';
import { fileURLToPath } from 'node:url';

const __dirname = dirname(fileURLToPath(import.meta.url));
const PLUGIN_ROOT = join(__dirname, '..', 'com.g0jkn.aethersdr.ulanziPlugin');
const OUT_DIR          = join(PLUGIN_ROOT, 'assets', 'icons');
const OUT_DIR_LAUNCHER = join(PLUGIN_ROOT, 'assets', 'launchers');

// ─── Palette ─────────────────────────────────────────────────────────────
// Eye-balled match for AetherSDR's dark-theme accent colours.  Five
// category swatches let the LCD keys group naturally by function so an
// operator can spot "all the gain controls" at a glance.
const PALETTE = {
  tx:     '#c0394b',   // TX / MOX               (red)
  tune:   '#d9853b',   // ATU tune cycle         (amber)
  rx:     '#1ea672',   // RX-side: mode/slice/rit (green)
  band:   '#3a78c2',   // band navigation         (blue)
  gain:   '#9b7fc2',   // AF / RF / mic gain      (purple)
  spec:   '#00b4d8',   // VFO / spectrum          (cyan — matches AetherSDR brand)
  bg:     '#0f0f1a',   // dark backdrop
  fg:     '#ffffff',
};

// ─── Action catalogue ────────────────────────────────────────────────────
// Single source of truth for icon look across the whole plugin.  Filename
// stem ↔ manifest.json icon path.  Keep label short (1–4 chars renders
// best); use `sub` for paired up/down or qualifier glyphs.
const ICONS = [
  // ── existing core actions ──
  { name: 'mox',         label: 'MOX',   color: 'tx'   },
  { name: 'tune',        label: 'TUNE',  color: 'tune' },
  { name: 'rit',         label: 'RIT',   color: 'rx'   },
  { name: 'mode',        label: 'MODE',  color: 'rx'   },
  { name: 'slice',       label: 'SLICE', color: 'rx'   },
  { name: 'band-up',     label: 'BAND',  color: 'band', sub: '▲' },
  { name: 'band-down',   label: 'BAND',  color: 'band', sub: '▼' },
  { name: 'vfo',         label: 'VFO',   color: 'spec' },

  // ── direct-mode actions (D200H pages prefer explicit over cycle) ──
  { name: 'mode-usb',    label: 'USB',   color: 'rx'   },
  { name: 'mode-lsb',    label: 'LSB',   color: 'rx'   },
  { name: 'mode-cw',     label: 'CW',    color: 'rx'   },
  { name: 'mode-digu',   label: 'DIGU',  color: 'rx'   },

  // ── gain trio (AF / RF / Mic × ▲/▼) ──
  { name: 'af-gain-up',    label: 'AF',  color: 'gain', sub: '▲' },
  { name: 'af-gain-down',  label: 'AF',  color: 'gain', sub: '▼' },
  { name: 'rf-gain-up',    label: 'RF',  color: 'gain', sub: '▲' },
  { name: 'rf-gain-down',  label: 'RF',  color: 'gain', sub: '▼' },
  { name: 'mic-gain-up',   label: 'MIC', color: 'gain', sub: '▲' },
  { name: 'mic-gain-down', label: 'MIC', color: 'gain', sub: '▼' },
];

// ─── Launcher catalogue ──────────────────────────────────────────────────
// Tiles for Studio's built-in System → Open action — Nigel binds these to
// shack-companion apps so a single D200H press launches TCI Monitor / log
// / capture / etc.  Deliberately different visual language from the radio-
// control action tiles: DARK background + ACCENT-coloured wordmark + small
// ↗ launcher glyph in the corner.  Operator can tell at a glance whether
// a key controls the radio (block of colour) or opens another app (dark
// tile with arrow).
const LAUNCHERS = [
  // accent  = wordmark colour (kept distinct per app for fast recognition)
  // lines   = two-line label (max ~6 chars per line for legibility @ 72px)
  { name: 'aethersdr',   lines: ['AETHER', 'SDR'], accent: '#00b4d8' },  // cyan — flagship
  { name: 'tci-monitor', lines: ['TCI',    'MON'], accent: '#56c6e8' },  // light cyan — debug sibling
  { name: 'shacklog',    lines: ['SHACK',  'LOG'], accent: '#1ea672' },  // green — logbook
  { name: 'iq-capture',  lines: ['IQ',     'CAP'], accent: '#e85d75' },  // red — recording
  { name: 'aether-pad',  lines: ['AETHER', 'PAD'], accent: '#9b7fc2' },  // purple — controller
];

// ─── Renderers ───────────────────────────────────────────────────────────

// Pick a label point-size that fills the tile without overflowing.  Empirical;
// matched against the D100H key dimensions (144px ≈ 72×72 logical with @2x).
function labelSizeFor(text) {
  if (text.length <= 2) return 64;
  if (text.length === 3) return 56;
  if (text.length === 4) return 46;
  return 38;
}

function renderActionIcon({ label, color, sub }) {
  const bg = PALETTE[color] ?? PALETTE.bg;
  const fg = PALETTE.fg;
  const lsize = labelSizeFor(label);
  // Centre the label vertically; nudge up when a sub-glyph is present.
  const labelY = sub ? 78 : 92;
  const subBlock = sub
    ? `\n  <text x="72" y="122" font-family="Inter, Helvetica Neue, Arial, sans-serif" font-size="30" fill="${fg}" text-anchor="middle" font-weight="600">${sub}</text>`
    : '';
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 144 144">
  <rect width="144" height="144" rx="20" fill="${bg}"/>
  <text x="72" y="${labelY}" font-family="Inter, Helvetica Neue, Arial, sans-serif" font-size="${lsize}" fill="${fg}" text-anchor="middle" font-weight="800" letter-spacing="1">${label}</text>${subBlock}
</svg>
`;
}

// Plugin master icon — shown in Studio's plugin picker.  Small spectrum-dot
// motif + AetherSDR wordmark.
function renderPluginIcon() {
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 144 144">
  <rect width="144" height="144" rx="20" fill="${PALETTE.bg}"/>
  <circle cx="72" cy="58" r="26" fill="none" stroke="${PALETTE.spec}" stroke-width="5"/>
  <circle cx="72" cy="58" r="11" fill="${PALETTE.spec}"/>
  <text x="72" y="116" font-family="Inter, Helvetica Neue, Arial, sans-serif" font-size="20" fill="${PALETTE.fg}" text-anchor="middle" font-weight="700" letter-spacing="0.5">AetherSDR</text>
</svg>
`;
}

// Launcher tile — dark backdrop, accent-coloured two-line wordmark, small
// up-right arrow in the corner.  Pairs with Studio's System → Open action.
function renderLauncherIcon({ lines, accent }) {
  const [l1, l2] = lines;
  const sizeFor = (text) => text.length <= 3 ? 42 : text.length <= 5 ? 36 : 30;
  const s1 = sizeFor(l1);
  const s2 = sizeFor(l2);
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 144 144">
  <rect width="144" height="144" rx="20" fill="${PALETTE.bg}"/>
  <rect x="2" y="2" width="140" height="140" rx="18" fill="none" stroke="${accent}" stroke-width="2" opacity="0.5"/>
  <text x="124" y="32" font-family="Inter, Helvetica Neue, Arial, sans-serif" font-size="22" fill="${accent}" text-anchor="middle" font-weight="700">↗</text>
  <text x="72" y="78"  font-family="Inter, Helvetica Neue, Arial, sans-serif" font-size="${s1}" fill="${accent}"   text-anchor="middle" font-weight="800" letter-spacing="0.5">${l1}</text>
  <text x="72" y="114" font-family="Inter, Helvetica Neue, Arial, sans-serif" font-size="${s2}" fill="${PALETTE.fg}" text-anchor="middle" font-weight="800" letter-spacing="0.5">${l2}</text>
</svg>
`;
}

// Category icon — used by Studio when grouping actions in the picker tree.
function renderCategoryIcon() {
  return `<svg xmlns="http://www.w3.org/2000/svg" viewBox="0 0 144 144">
  <rect width="144" height="144" rx="20" fill="${PALETTE.bg}"/>
  <text x="72" y="92" font-family="Inter, Helvetica Neue, Arial, sans-serif" font-size="72" fill="${PALETTE.spec}" text-anchor="middle" font-weight="900">AE</text>
</svg>
`;
}

// ─── Main ────────────────────────────────────────────────────────────────
mkdirSync(OUT_DIR, { recursive: true });
mkdirSync(OUT_DIR_LAUNCHER, { recursive: true });

console.log('Action icons:');
for (const icon of ICONS) {
  writeFileSync(join(OUT_DIR, `${icon.name}.svg`), renderActionIcon(icon));
  console.log(`  ${icon.name.padEnd(18)} ${icon.color.padEnd(6)} ${icon.label}${icon.sub ? ' ' + icon.sub : ''}`);
}
writeFileSync(join(OUT_DIR, 'pluginIcon.svg'),  renderPluginIcon());
writeFileSync(join(OUT_DIR, 'category.svg'),    renderCategoryIcon());

console.log('\nLauncher tiles (for Studio\'s System → Open action):');
for (const tile of LAUNCHERS) {
  writeFileSync(join(OUT_DIR_LAUNCHER, `${tile.name}.svg`), renderLauncherIcon(tile));
  console.log(`  ${tile.name.padEnd(18)} ${tile.accent}  ${tile.lines.join(' ')}`);
}

console.log(`\nGenerated ${ICONS.length + 2} action icons → ${OUT_DIR}`);
console.log(`Generated ${LAUNCHERS.length} launcher tiles → ${OUT_DIR_LAUNCHER}`);
