"""Motor driver abstraction for differential drive rover."""

import logging

logger = logging.getLogger(__name__)


class MotorDriver:
    """Controls left and right drive motors via GPIO PWM."""

    def __init__(self, left_pin: int, right_pin: int):
        self.left_pin = left_pin
        self.right_pin = right_pin
        self._left_speed = 0.0
        self._right_speed = 0.0
        self._setup()

    def _setup(self):
        try:
            import RPi.GPIO as GPIO
            GPIO.setmode(GPIO.BCM)
            GPIO.setup(self.left_pin, GPIO.OUT)
            GPIO.setup(self.right_pin, GPIO.OUT)
            self._pwm_left = GPIO.PWM(self.left_pin, 1000)
            self._pwm_right = GPIO.PWM(self.right_pin, 1000)
            self._pwm_left.start(0)
            self._pwm_right.start(0)
            self._gpio = GPIO
            logger.info("GPIO motor driver initialized")
        except ImportError:
            logger.warning("RPi.GPIO not available — running in simulation mode")
            self._gpio = None

    def set_speeds(self, left: float, right: float):
        """Set motor speeds in range [-1.0, 1.0]."""
        self._left_speed = max(-1.0, min(1.0, left))
        self._right_speed = max(-1.0, min(1.0, right))

        if self._gpio:
            self._pwm_left.ChangeDutyCycle(abs(self._left_speed) * 100)
            self._pwm_right.ChangeDutyCycle(abs(self._right_speed) * 100)

        logger.debug("Motors: left=%.2f right=%.2f", self._left_speed, self._right_speed)

    def stop(self):
        self.set_speeds(0.0, 0.0)

    def cleanup(self):
        self.stop()
        if self._gpio:
            self._pwm_left.stop()
            self._pwm_right.stop()
            self._gpio.cleanup()
