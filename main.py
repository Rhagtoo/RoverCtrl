#!/usr/bin/env python3
"""RoverCtrl - Main entry point for rover control system."""

import time
import signal
import sys
from rover.controller import RoverController
from rover.config import Config


def signal_handler(sig, frame):
    print("\nShutting down rover...")
    sys.exit(0)


def main():
    signal.signal(signal.SIGINT, signal_handler)
    signal.signal(signal.SIGTERM, signal_handler)

    config = Config.load("config.yaml")
    rover = RoverController(config)

    print("RoverCtrl started. Press Ctrl+C to stop.")
    rover.start()

    try:
        while True:
            rover.update()
            time.sleep(config.loop_interval)
    finally:
        rover.stop()


if __name__ == "__main__":
    main()
