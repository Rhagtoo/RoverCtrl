"""Rover configuration management."""

import yaml
from dataclasses import dataclass, field
from typing import Optional


@dataclass
class Config:
    loop_interval: float = 0.05  # seconds (20 Hz)
    max_speed: float = 1.0       # normalized 0..1
    max_turn_rate: float = 0.8   # normalized 0..1
    motor_left_pin: int = 17
    motor_right_pin: int = 27
    servo_pan_pin: int = 22
    servo_tilt_pin: int = 23
    telemetry_port: int = 5000
    telemetry_host: str = "0.0.0.0"
    log_level: str = "INFO"

    @classmethod
    def load(cls, path: str) -> "Config":
        try:
            with open(path, "r") as f:
                data = yaml.safe_load(f) or {}
            return cls(**{k: v for k, v in data.items() if hasattr(cls, k)})
        except FileNotFoundError:
            return cls()
