"""High-level rover controller — ties together motors, sensors, telemetry."""

import logging
import time
from .config import Config
from .motors import MotorDriver
from .sensors import UltrasonicSensor
from .telemetry import TelemetryServer

logger = logging.getLogger(__name__)

OBSTACLE_DISTANCE = 0.3  # metres — stop if closer than this


class RoverController:
    def __init__(self, config: Config):
        self.config = config
        self.motors = MotorDriver(config.motor_left_pin, config.motor_right_pin)
        self.ultrasonic = UltrasonicSensor(trigger_pin=24, echo_pin=25)
        self.telemetry = TelemetryServer(config.telemetry_host, config.telemetry_port)
        self._target_linear = 0.0
        self._target_angular = 0.0

    def start(self):
        logging.basicConfig(level=self.config.log_level)
        self.telemetry.start()
        logger.info("Rover started")

    def set_velocity(self, linear: float, angular: float):
        """Command rover with linear [-1,1] and angular [-1,1] velocity."""
        self._target_linear = max(-1.0, min(1.0, linear))
        self._target_angular = max(-1.0, min(1.0, angular))

    def update(self):
        distance = self.ultrasonic.read_distance()

        if distance is not None and distance < OBSTACLE_DISTANCE and self._target_linear > 0:
            logger.warning("Obstacle at %.2f m — stopping", distance)
            left, right = 0.0, 0.0
        else:
            left, right = self._mix(self._target_linear, self._target_angular)

        self.motors.set_speeds(left * self.config.max_speed,
                               right * self.config.max_speed)

        self.telemetry.send({
            "ts": time.time(),
            "linear": self._target_linear,
            "angular": self._target_angular,
            "distance_m": distance,
            "motor_left": left,
            "motor_right": right,
        })

    def _mix(self, linear: float, angular: float):
        """Differential drive mixing."""
        left = linear + angular * self.config.max_turn_rate
        right = linear - angular * self.config.max_turn_rate
        scale = max(1.0, abs(left), abs(right))
        return left / scale, right / scale

    def stop(self):
        self.motors.cleanup()
        self.telemetry.stop()
        logger.info("Rover stopped")
