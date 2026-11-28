# Raspberry Pi drive bench

Stage 1 of the [Pi design brief](../../raspberry-pi-design-brief.md) runs on Ubuntu Server 24.04 with Python and pySerial. It exercises USB protocol v1 on the [ESP32 DevKit V1 and Waveshare ESP32-S3-Zero drive controller](../pi-drive-controller/README.md).

See [LD19 mapping on the Pi](slam/README.md) for the ROS 2 Jazzy SLAM launch and map-saving commands.

## Connect with motor power off

1. Install pySerial on the Pi with `sudo apt install python3-serial` if needed.
2. With the ESP32 powered over a data-capable USB cable, run `ls -l /dev/serial/by-path/` and find its port. The DevKit typically uses `ttyUSB` and the S3 typically uses `ttyACM`. Use the physical USB path when the LD19 and ESP32 adapters share the same CP2102 serial number. The `by-id` entry can point to the LiDAR instead. If port access is restricted, add your account to the `dialout` group and reconnect over SSH.
3. Close other serial clients. Set `PORT` to the identified port, then run these commands from the repo root. On a standalone copy, substitute the path to `bench.py`.

```sh
PORT=/dev/serial/by-path/your-esp32-port
python3 firmware/raspberry-pi/bench.py "$PORT" status
python3 firmware/raspberry-pi/bench.py "$PORT" watch --seconds 60
```

Check that status shows protocol v1, a boot ID, and `armed=False`. The default S3 build also reports `uncalibrated`. Turn each wheel several known revolutions by hand in both directions. Record the signed count change divided by revolutions for each wheel. After setting the firmware's Hall polarity, a chassis-forward turn should increase its wheel count.

## Web bench

On the Pi, `python3 firmware/raspberry-pi/bench_web.py "$PORT"` serves the dashboard on `127.0.0.1:8719`. Run either the web bench or the serial CLI at a time so each has exclusive access to the board. On the installed Pi, use `sudo systemctl stop hotelbot-wheel-bench` to free the serial port and `sudo systemctl start hotelbot-wheel-bench` to restore the dashboard.

With the Pi and workstation on the same Ethernet link, connect with `ssh hotelbot-pi.local`. The Pi advertises this name through Avahi (Bonjour), so it works as the link-local address changes. Forward the Pi's loopback port over SSH and open [the wheel bench](http://127.0.0.1:8719).

```sh
ssh -o ExitOnForwardFailure=yes \
  -N -L 127.0.0.1:8719:127.0.0.1:8719 hotelbot-pi.local
```

Set a signed target for each wheel, confirm both wheels are secured off the ground, and hold **Hold to run**. Release to stop, or use **Stop** or Escape. Each hold is limited to four seconds, with a 300 ms browser heartbeat deadline on the Pi and a separate firmware watchdog. Live plots show the previous 12 seconds of target and measured speed for each wheel. Hall counts remain visible without enabling motion.

## Remote control

Open [Remote control](http://127.0.0.1:8719/drive) on the same SSH tunnel. Confirm that the robot is in view and the area is clear, then drag the joystick or hold arrow keys or WASD. Horizontal input alone pivots the robot. Vertical input drives forward or in reverse. The Pi mixes the axes as `left = y + x` and `right = y − x` and scales the pair to fit the throttle. Throttle sets the target ceiling in RPM. The updated USB firmware advertises 200 RPM, and the earlier USB firmware caps targets at 40 RPM.

Joystick driving continues while held and receiving updates. Release, press Escape or tap **Stop** to cut drive. Switching away from the page clears the operator confirmation. A stale browser heartbeat expires after 300 ms, and the independent ESP32 watchdog disarms the board after 250 ms without a valid serial command. When the same controller recovers from a stop, release the joystick and press again once the page shows **Ready**. The dashboard keeps the previous stop reason visible until the next hold and logs it to the Pi’s journal. Keep the robot in view and allow room to coast.

The remote control and wheel bench share one ESP32 connection and one hold at a time. The wheel bench retains its four-second test limit.

## Powered tests with both wheels secured clear of the ground

Complete the [ESP32 calibration checks](../pi-drive-controller/README.md#commissioning) first. Confirm that `status` reports `ready=True` with no faults. Keep the motor-power disconnect within reach. Positive targets turn a wheel chassis-forward. Press Ctrl+C to abort a running test. The tool attempts an explicit stop on interruption, and the board watchdog handles loss of commands. Zero throttle allows the wheels to coast.

```sh
python3 firmware/raspberry-pi/bench.py "$PORT" speed --left 60 --right 0 --seconds 1 --wheels-lifted
python3 firmware/raspberry-pi/bench.py "$PORT" speed --left 0 --right -60 --seconds 1 --wheels-lifted
python3 firmware/raspberry-pi/bench.py "$PORT" move --left 45 --right -45 --max-deg-s 60 --wheels-lifted
python3 firmware/raspberry-pi/bench.py "$PORT" stop
python3 firmware/raspberry-pi/bench.py "$PORT" watchdog --left 60 --right 60 --wheels-lifted
python3 firmware/raspberry-pi/bench.py "$PORT" disconnect --left 60 --right 60 --wheels-lifted
# Reconnect USB and confirm the port path, then check the new session.
python3 firmware/raspberry-pi/bench.py "$PORT" status
```

The paired-move test requires a firmware build that advertises `move_wheels`. It sends one relative target and keeps that move alive until the board reports completion. For the watchdog check, the tool sends one speed target and waits for a `link_timeout` disarm. The reported time includes telemetry delay. For the disconnect check, unplug USB when prompted. If the cable also powers the ESP32, record that the controller lost power during this check. Use the separate watchdog test to verify command-loss behavior with the controller powered.

| Check | Pass condition | Bench record |
|---|---|---|
| USB and hand turns | Disarmed handshake. Each wheel reports positive forward counts and negative reverse counts after calibration. | USB handshake passed on the DevKit (CP2102, 115200). Received 51 snapshots at 25.0 Hz. The earlier Hall-C-only measurement of 15 rising edges per revolution predicts 90 transitions with all three channels. The initial forward hand turn changed the raw counts by left +10 with a Hall fault and right −91. A later three-turn left test measured −271 after the fault cleared. Retest with the calibrated firmware. |
| Uncalibrated motion lockout | The ESP32 refuses arm and speed commands, and still accepts stop. | Passed. Board reported `uncalibrated`, rejected arm with `cannot_arm` and zero speed with `cannot_drive`, and confirmed stop with `armed=False`. |
| Individual wheel speed and stop | Only the selected wheel runs, its measured direction agrees with the command, and stop cuts throttle and disarms. | Pending. Record targets, directions, and stop observations. |
| Paired move | A single request reaches the requested wheel positions within the firmware tolerance and ends idle and disarmed. | Pending. Record goals and measured count changes. |
| Watchdog | With the ESP32 powered, PWM drops to zero on command loss and telemetry reports `link_timeout`. | Pending. Measure PWM cutoff against the firmware's 250 ms timeout. |
| Disconnect and reconnect | Drive output drops on cable loss. New session reports idle and disarmed, and the wheels stay still until a new explicit arm. | Pending. Record cutoff, reconnect state, and whether USB also supplied power. |

During the left-wheel retest, the Pi logged a CP2102 USB disconnect and a brief undervoltage alarm. The ESP32 returned with a new boot ID and both counters at zero, so the retest did not yield a continuous count delta. After reconnection, `left_hall` was still present at rest and `stop` confirmed idle and disarmed. No powered motion was tested.

On the next retry, the same boot reported a left-count change from 0 to −97 and `left_hall` cleared. A subsequent 55-second capture received 1,375 snapshots without `left_hall` or a gap longer than 120 ms.

After identifying the ESP32 by its physical USB path, the left count changed from −8 to −279 over three confirmed forward revolutions on the same boot. The DevKit source now uses 90 transitions per revolution for both wheels, reverses both Hall signs so forward counts are positive, and uses the existing DevKit Rotation levels of left LOW and right HIGH. Confirm the signed readings again after flashing before powered tests.

Flashed the calibrated DevKit application through the Pi with `Kp=0.00089` and `Ki=0.0055` on both wheels. esptool verified the written image, and the Pi bench tool confirmed `ready=True`, `armed=False`, and no faults after reboot. The previous application is backed up on the Pi. The powered checks remain pending.
