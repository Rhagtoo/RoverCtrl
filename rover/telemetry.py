"""UDP telemetry server — streams rover state as JSON datagrams."""

import json
import socket
import threading
import logging
from typing import Dict, Any

logger = logging.getLogger(__name__)


class TelemetryServer:
    def __init__(self, host: str, port: int):
        self.host = host
        self.port = port
        self._sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self._clients: set = set()
        self._lock = threading.Lock()
        self._running = False

    def start(self):
        self._running = True
        self._thread = threading.Thread(target=self._listen, daemon=True)
        self._thread.start()
        logger.info("Telemetry server listening on %s:%d", self.host, self.port)

    def _listen(self):
        """Accept registration packets from clients."""
        recv_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        recv_sock.bind((self.host, self.port))
        recv_sock.settimeout(1.0)
        while self._running:
            try:
                data, addr = recv_sock.recvfrom(64)
                if data.strip() == b"REGISTER":
                    with self._lock:
                        self._clients.add(addr)
                    logger.debug("Client registered: %s", addr)
            except socket.timeout:
                continue

    def send(self, state: Dict[str, Any]):
        payload = json.dumps(state).encode()
        with self._lock:
            dead = set()
            for addr in self._clients:
                try:
                    self._sock.sendto(payload, addr)
                except OSError:
                    dead.add(addr)
            self._clients -= dead

    def stop(self):
        self._running = False
