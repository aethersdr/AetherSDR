// The docs sidebar. Edit it by hand: every page in docs/ belongs in it.
// The categories in sidebar-extra.json (the pages tools/docs/gen_reference.py
// generates) are appended last. A release snapshot freezes the result into
// versioned_sidebars/ (tools/docs/snapshot_stable.py).

import {createRequire} from 'node:module';

const require = createRequire(import.meta.url);
const extra = require('./sidebar-extra.json');

/** @type {import('@docusaurus/plugin-content-docs').SidebarsConfig} */
const sidebars = {
  "docs": [
    {
      "type": "doc",
      "id": "index",
      "label": "Home"
    },
    {
      "type": "category",
      "label": "Start Here",
      "items": [
        "your-first-session",
        "installation",
        "first-connection",
        "before-you-transmit"
      ]
    },
    {
      "type": "category",
      "label": "Your Radio",
      "items": [
        "supported-radios",
        "flexradio",
        "hermes-lite-2",
        "networked-icom",
        "anan-g2",
        "rtl-sdr",
        "kiwisdr-and-web-888",
        "demo-mode"
      ]
    },
    {
      "type": "category",
      "label": "Your Computer",
      "items": [
        "linux",
        "macos",
        "windows"
      ]
    },
    {
      "type": "category",
      "label": "Operating",
      "items": [
        "panadapter-controls",
        "vfo-widget",
        "rx-controls",
        "tx-controls",
        "meters",
        "aetherial-audio",
        "multi-slice-operation",
        "split-operation",
        "diversity-and-esc",
        "tnf-tracking-notch-filters",
        "memory-channels",
        "profile-management",
        "xvtr-transverters",
        "workspace-canvas"
      ]
    },
    {
      "type": "category",
      "label": "CW & Digital Modes",
      "items": [
        "cwx-panel",
        "cw-decoder",
        "dvk-panel",
        "rtty-operation",
        "rade-digital-voice",
        "d-star-thumbdv",
        "aethermodem-packet-radio",
        "copy-assist",
        "wsjt-x-integration"
      ]
    },
    {
      "type": "category",
      "label": "Connecting Other Software",
      "items": [
        "dax-virtual-audio",
        "dax-iq-streaming",
        "cat-control",
        "tci-server",
        "mqtt-station-automation",
        "automation-bridge-and-mcp"
      ]
    },
    {
      "type": "category",
      "label": "Spotting & Tools",
      "items": [
        "spothub",
        "psk-reporter-map",
        "aethersweep",
        "net-scheduler",
        "aetherclock-and-gps",
        "callsign-lookup"
      ]
    },
    {
      "type": "category",
      "label": "Station Accessories",
      "items": [
        "peripherals",
        "amplifiers",
        "tgxl-tuner-control",
        "shackswitch",
        "green-heron-everyware",
        "usb-cable-management"
      ]
    },
    {
      "type": "category",
      "label": "Controllers",
      "items": [
        "streamdeck",
        "flexcontrol-tuning-knob",
        "ulanzi-dial",
        "usb-control-surfaces",
        "midi-controller-mapping",
        "ctr2-proxy"
      ]
    },
    {
      "type": "category",
      "label": "Remote Operation",
      "items": [
        "smartlink-setup",
        "manual-connection",
        "tailscale-remote-access",
        "low-bandwidth-connections",
        "multi-flex"
      ]
    },
    {
      "type": "category",
      "label": "Settings",
      "items": [
        "radio-setup",
        "audio-settings",
        "settings-and-backups",
        "keyboard-shortcuts",
        "themes-and-theme-editor",
        "slice-colors",
        "accessibility",
        "firmware-update"
      ]
    },
    {
      "type": "category",
      "label": "Reference",
      "items": [
        "menu-reference",
        "keyboard-shortcuts"
      ]
    },
    {
      "type": "category",
      "label": "Understanding",
      "items": [
        "dsp-noise-mitigation",
        "bnr-gpu-noise-removal",
        "nr2-noise-reduction",
        "gpu-rendering"
      ]
    },
    {
      "type": "category",
      "label": "Help",
      "items": [
        "troubleshooting",
        "support-and-logging",
        "runtime-monitor"
      ]
    },
    {
      "type": "category",
      "label": "Contributing",
      "items": [
        "contributing-guide",
        "docs-style-guide",
        "translating-the-docs",
        "ai-assisted-development",
        "building-from-source"
      ]
    }
  ].concat(extra.map((category) => ({type: 'category', ...category})))
};

export default sidebars;
