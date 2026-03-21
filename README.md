# RoverCtrl

Python-based control system for a differential-drive rover (Raspberry Pi).

## Structure

```
RoverCtrl/
├── main.py              # Entry point
├── config.yaml          # Runtime configuration
├── requirements.txt
└── rover/
    ├── config.py        # Config dataclass
    ├── controller.py    # High-level rover controller
    ├── motors.py        # GPIO PWM motor driver
    ├── sensors.py       # Ultrasonic distance sensor
    └── telemetry.py     # UDP telemetry server
```

## Quick start

```bash
pip install -r requirements.txt
python main.py
```

## Configuration

Edit `config.yaml` to adjust GPIO pins, speed limits, and telemetry settings.

## Telemetry

The rover streams JSON state datagrams over UDP (default port 5000).
Send a `REGISTER` packet to the telemetry port to subscribe.
