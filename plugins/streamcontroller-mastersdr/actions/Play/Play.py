"""
Play action for MasterSDR StreamController plugin.
Toggles slice recording playback (DVK voice keyer).
Uses MasterSDR-specific TCI extension: rx_play:<trx>,<on>;
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class Play(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._playing = False

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "play.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return

        tci = self.plugin_base.tci
        self._playing = not self._playing
        tci.send(f"rx_play:0,{str(self._playing).lower()};")
