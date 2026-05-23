"""
LockToggle action — toggle tune lock.
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class LockToggle(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._locked = False

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "lock.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event != Input.Key.Events.DOWN:
            return
        self._locked = not self._locked
        self.plugin_base.tci.send(f"lock:0,{str(self._locked).lower()};")
