"""
Shared TCI WebSocket client for MasterSDR StreamController plugin.
Connects to MasterSDR's TCI server (default ws://localhost:50001)
and provides send/receive for radio control commands.
"""

import threading
import time
from loguru import logger as log

try:
    import websocket
except ImportError:
    websocket = None
    log.warning("MasterSDR: websocket-client not installed — pip install websocket-client")


class TciClient:
    """Singleton TCI WebSocket client shared across all actions."""

    _instance = None
    _lock = threading.Lock()

    def __new__(cls):
        with cls._lock:
            if cls._instance is None:
                cls._instance = super().__new__(cls)
                cls._instance._initialized = False
            return cls._instance

    def __init__(self):
        if self._initialized:
            return
        self._initialized = True
        self._ws = None
        self._connected = False
        self._thread = None
        self._host = "localhost"
        self._port = 50001
        self._callbacks = []
        self._reconnect = True

    def connect(self, host="localhost", port=50001):
        self._host = host
        self._port = port
        self._reconnect = True
        if self._thread and self._thread.is_alive():
            return
        self._thread = threading.Thread(target=self._run, daemon=True)
        self._thread.start()

    def disconnect(self):
        self._reconnect = False
        if self._ws:
            self._ws.close()

    def send(self, command: str):
        """Send a TCI command (e.g. 'trx:0,true;')"""
        if self._ws and self._connected:
            try:
                self._ws.send(command)
            except Exception as e:
                log.error(f"MasterSDR TCI send error: {e}")

    def is_connected(self) -> bool:
        return self._connected

    def add_callback(self, cb):
        self._callbacks.append(cb)

    def _run(self):
        while self._reconnect:
            try:
                url = f"ws://{self._host}:{self._port}"
                log.info(f"MasterSDR: connecting to TCI at {url}")
                self._ws = websocket.WebSocketApp(
                    url,
                    on_open=self._on_open,
                    on_message=self._on_message,
                    on_close=self._on_close,
                    on_error=self._on_error,
                )
                self._ws.run_forever()
            except Exception as e:
                log.error(f"MasterSDR TCI error: {e}")
            self._connected = False
            if self._reconnect:
                time.sleep(3)

    def _on_open(self, ws):
        self._connected = True
        log.info("MasterSDR: TCI connected")

    def _on_message(self, ws, message):
        for cb in self._callbacks:
            try:
                cb(message)
            except Exception as e:
                log.error(f"MasterSDR TCI callback error: {e}")

    def _on_close(self, ws, code, msg):
        self._connected = False
        log.info(f"MasterSDR: TCI disconnected ({code})")

    def _on_error(self, ws, error):
        log.error(f"MasterSDR TCI error: {error}")
