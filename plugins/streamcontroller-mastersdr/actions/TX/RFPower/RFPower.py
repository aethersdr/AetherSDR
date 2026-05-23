"""
RFPower action — adjust RF drive power.
Supports both key press (step +/-5) and dial rotation on StreamDeck+.
"""

import os
from src.backend.PluginManager.ActionBase import ActionBase
from src.backend.DeckManagement.InputIdentifier import Input


class RFPower(ActionBase):
    def __init__(self, *args, **kwargs):
        super().__init__(*args, **kwargs)
        self._power = 50  # percent

    def on_ready(self):
        icon = os.path.join(self.plugin_base.PATH, "assets", "rf_power.png")
        if os.path.exists(icon):
            self.set_media(media_path=icon)

    def event_callback(self, event, data):
        if event == Input.Dial.Events.TURN_CW:
            self._power = min(100, self._power + 5)
            self.plugin_base.tci.send(f"drive:{self._power};")
        elif event == Input.Dial.Events.TURN_CCW:
            self._power = max(0, self._power - 5)
            self.plugin_base.tci.send(f"drive:{self._power};")
        elif event == Input.Key.Events.DOWN:
            # Key press cycles: 25 → 50 → 75 → 100 → 25
            self._power = (self._power + 25) % 125
            if self._power == 0:
                self._power = 25
            self.plugin_base.tci.send(f"drive:{self._power};")
