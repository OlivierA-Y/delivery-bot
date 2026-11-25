# HotelBot documentation

## System and Waveshare ESP32-S3-Zero

- [System diagram](models/motor-mount/system-diagram.html) — Pi, ESP32, power and the proposed USB control interface.
- [Full wiring schematic](models/motor-mount/full-wiring.html) — battery, both motors, six Hall taps, connector pinouts and Pi peripherals.
- [Waveshare wiring and setup guide](models/motor-mount/wiring-guide.html) — GPIO assignments, signal circuits and bench checks.

## ESP32 DevKit V1 and bench tools

- [DevKit V1 wiring and PID tuning](models/motor-mount/devkit-v1-wiring.html) — pinout for the classic 30-pin ESP32 board.
- [Drive-controller firmware guide](firmware/drive-controller/README.md) — Wi-Fi joystick, Hall calibration, PID tuning and the USB bench program.
- [Motor sweep guide](firmware/motor-sweep/README.md) — single-wheel PWM test for the ESP32-S3-Zero and classic ESP32.
- [ESP32 pin plotter guide](tools/pin-plotter/README.md) — GPIO signal monitoring over USB serial.

## Motor mount

- [Interactive motor-mount CAD](models/motor-mount/motor-mount-3d.html) — axle clamp and mounting plate model.
- [Motor-mount design notes](models/motor-mount/README.md) — measurements, fastener layout and model rebuild instructions.

GitHub shows the source for HTML pages. For the interactive diagrams, open [documentation.html](documentation.html) in your browser from a local checkout.
