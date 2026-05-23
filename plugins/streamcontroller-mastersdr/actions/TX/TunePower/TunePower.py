"""
TunePower action — adjust tune drive power.
Supports both key press (step +/-5) and dial rotation on StreamDeck+.
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class TunePower(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._power = 10  # percent (tune usually lower)

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "tune_power.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event == Input.Dial.Events.TURN_CW:
            self._power = min(100, self._power + 5)
            self.plugin_base.tci.send(f"tune_drive:{self._power};")
        elif event == Input.Dial.Events.TURN_CCW:
            self._power = max(0, self._power - 5)
            self.plugin_base.tci.send(f"tune_drive:{self._power};")
        elif event == Input.Key.Events.DOWN:
            self._power = (self._power + 10) % 110
            if self._power == 0:
                self._power = 10
            self.plugin_base.tci.send(f"tune_drive:{self._power};")
