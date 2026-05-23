"""
MuteToggle action — toggle master audio mute.
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class MuteToggle(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._muted = False

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "mute.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return
        self._muted = not self._muted
        self.plugin_base.tci.send(f"mute:{str(self._muted).lower()};")
