"""
VolumeDown action — decrease master volume by 5.
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class VolumeDown(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._volume = 50

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "volume_down.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return
        self._volume = max(0, self._volume - 5)
        self.plugin_base.tci.send(f"volume:{self._volume};")
