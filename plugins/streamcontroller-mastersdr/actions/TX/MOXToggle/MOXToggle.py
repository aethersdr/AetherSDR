"""
MOXToggle action — toggle transmit on/off (unlike PTT which is hold-to-talk).

The third argument on the TCI ``trx:`` command tells MasterSDR which audio
source to use.  Valid values in our implementation:

    ``micpc`` — PC microphone (no DAX activation)
    ``mic``   — Radio-side mic (whatever mic_selection is set to on the radio)
    ``bal``   — Balanced input

``dax`` (or omitting arg3) activates DAX TX, which is intended only for
applications like WSJT-X/JTDX that actually feed audio over the DAX stream.
MOX/PTT buttons should never default to DAX (#1534).
"""

import os

from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Gtk, Adw


# (user-visible label, TCI source token) — order defines combo order; index 0
# is the default.
SOURCE_OPTIONS = [
    ("PC Mic",   "micpc"),
    ("Radio Mic", "mic"),
    ("Balanced", "bal"),
]


class MOXToggle(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._transmitting = False

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "mox.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return
        self._transmitting = not self._transmitting
        source = self._selected_source()
        self.plugin_base.tci.send(
            f"trx:0,{str(self._transmitting).lower()},{source};"
        )

    # ── Config UI ────────────────────────────────────────────────────────

    def get_config_rows(self) -> list:
        rows = super().get_config_rows()

        self._source_model = Gtk.StringList()
        for label, _ in SOURCE_OPTIONS:
            self._source_model.append(label)

        self._source_row = Adw.ComboRow(
            model=self._source_model,
            title="TX Audio Source",
            subtitle="Which mic path the radio uses when MOX toggles TX on",
        )
        self._load_selected_source()
        self._source_row.connect("notify::selected", self._on_change_source)

        rows.append(self._source_row)
        return rows

    def _load_selected_source(self):
        settings = self.get_settings() or {}
        saved_token = settings.get("source", SOURCE_OPTIONS[0][1])
        for i, (_, token) in enumerate(SOURCE_OPTIONS):
            if token == saved_token:
                self._source_row.set_selected(i)
                return
        self._source_row.set_selected(0)

    def _on_change_source(self, *args):
        idx = self._source_row.get_selected()
        if idx < 0 or idx >= len(SOURCE_OPTIONS):
            return
        settings = self.get_settings() or {}
        settings["source"] = SOURCE_OPTIONS[idx][1]
        self.set_settings(settings)

    def _selected_source(self) -> str:
        settings = self.get_settings() or {}
        return settings.get("source", SOURCE_OPTIONS[0][1])
