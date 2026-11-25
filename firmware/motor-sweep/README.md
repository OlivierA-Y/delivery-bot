# ZS-X11H V2 motor sweep

This is a bench test for the delivery bot’s ESP32. It supports the Waveshare ESP32-S3-Zero and classic ESP32 development boards with 4 MB of flash. It sends 490 Hz PWM, matching the [Oracid reference sketch](https://github.com/oracid/PID-for-Hoverboard-motor-with-ZS-X11H-controller/blob/main/MinMax.ino).

| ESP32 | ZS-X11H V2 |
| --- | --- |
| GPIO15 | PWM throttle |
| GND | Signal ground |

Follow the [wiring guide](../../models/motor-mount/wiring-guide.html) for phase and Hall wiring. Fit the external PWM jumper and set the driver’s onboard speed pot to minimum. Verify the PWM input is compatible with the ESP32’s 3.3 V output and that the PWM pull-down holds it low during reset. Lift the wheel clear of the bench and keep the motor power cutoff within reach.

From this directory, build and upload with PlatformIO, then open the serial monitor. Use `esp32dev` for the classic ESP32 connected through a CP2102 USB-to-UART bridge. Use `waveshare_esp32_s3_zero` for the Waveshare board and connect through its USB-C data port.

```sh
pio run -e esp32dev
pio run -e esp32dev -t upload
pio device monitor -e esp32dev
```

Open serial at 115200 baud. The output starts at 0%. Send `s` to start a repeating cycle. Each cycle ramps from 0% to 100% duty over five seconds, holds for half a second, ramps back to 0% over five seconds, then pauses at 0% for half a second. Send `x` to stop the cycle and return to 0% immediately. Send `?` to list commands.

The sweep controls throttle duty. Actual RPM depends on the motor, supply and load, and the wheel can coast after throttle returns to zero. The controller’s Rotation input sets direction.
