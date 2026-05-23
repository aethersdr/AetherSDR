"""
Record action for MasterSDR StreamController plugin.
Toggles slice recording (DVK voice keyer record).
Uses MasterSDR-specific TCI extension: rx_record:<trx>,<on>;
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class Record(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._recording = False

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "record.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return

        tci = self.plugin_base.tci
        self._recording = not self._recording
        tci.send(f"rx_record:0,{str(self._recording).lower()};")
