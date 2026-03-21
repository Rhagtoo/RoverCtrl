"""Sensor interface: IMU, ultrasonic distance, battery voltage."""

import logging
import time

logger = logging.getLogger(__name__)


class UltrasonicSensor:
    """HC-SR04 ultrasonic distance sensor."""

    SPEED_OF_SOUND = 343.0  # m/s at ~20°C

    def __init__(self, trigger_pin: int, echo_pin: int, timeout: float = 0.05):
        self.trigger_pin = trigger_pin
        self.echo_pin = echo_pin
        self.timeout = timeout
        self._gpio = None
        self._setup()

    def _setup(self):
        try:
            import RPi.GPIO as GPIO
            GPIO.setup(self.trigger_pin, GPIO.OUT, initial=GPIO.LOW)
            GPIO.setup(self.echo_pin, GPIO.IN)
            self._gpio = GPIO
        except ImportError:
            logger.warning("RPi.GPIO not available — ultrasonic in simulation mode")

    def read_distance(self) -> Optional[float]:
        """Return distance in metres, or None on timeout."""
        if not self._gpio:
            return 0.5  # simulated value

        GPIO = self._gpio
        GPIO.output(self.trigger_pin, GPIO.HIGH)
        time.sleep(10e-6)
        GPIO.output(self.trigger_pin, GPIO.LOW)

        start = time.time()
        while GPIO.input(self.echo_pin) == 0:
            if time.time() - start > self.timeout:
                return None
        pulse_start = time.time()

        while GPIO.input(self.echo_pin) == 1:
            if time.time() - pulse_start > self.timeout:
                return None
        pulse_end = time.time()

        return (pulse_end - pulse_start) * self.SPEED_OF_SOUND / 2


try:
    from typing import Optional
except ImportError:
    pass
