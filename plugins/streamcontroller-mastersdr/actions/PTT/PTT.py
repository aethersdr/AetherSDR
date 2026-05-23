"""
PTT (Push-to-Talk) action for MasterSDR StreamController plugin.
Hold the button/pedal to transmit, release to return to receive.

See ``MOXToggle.py`` for the source-token reference — same options here.
"""

import os

from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input

import gi
gi.require_version("Gtk", "4.0")
gi.require_version("Adw", "1")
from gi.repository import Gtk, Adw


SOURCE_OPTIONS = [
    ("PC Mic",   "micpc"),
    ("Radio Mic", "mic"),
    ("Balanced", "bal"),
]


class PTT(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "ptt.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        tci = self.plugin_base.tci
        source = self._selected_source()

        if event == Input.Key.Events.DOWN:
            tci.send(f"trx:0,true,{source};")
        elif event == Input.Key.Events.UP:
            tci.send(f"trx:0,false,{source};")

    # ── Config UI ────────────────────────────────────────────────────────

    def get_config_rows(self) -> list:
        rows = super().get_config_rows()

        self._source_model = Gtk.StringList()
        for label, _ in SOURCE_OPTIONS:
            self._source_model.append(label)

        self._source_row = Adw.ComboRow(
            model=self._source_model,
            title="TX Audio Source",
            subtitle="Which mic path the radio uses while PTT is held",
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
