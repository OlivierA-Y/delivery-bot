# HotelBot Drive

The ESP32 serves a phone controller over its own Wi-Fi access point. Use the joystick or enter signed RPM targets for each wheel. Each wheel uses its Hall pulses to adjust PWM through a separate PID loop.

Open the [DevKit V1 wiring and PID tuning guide](../../models/motor-mount/devkit-v1-wiring.html) for the full diagrams and connector layout.

## Wiring

| Wheel | PWM | Rotation | Hall C feedback | Brake ground switch |
| --- | --- | --- | --- | --- |
| Left | D15 / GPIO15 | D4 / GPIO4 | D22 / GPIO22 | D12 / GPIO12 |
| Right | RX2 / GPIO16 | D23 / GPIO23 | D18 / GPIO18 | D13 / GPIO13 |

Each Hall input counts rising edges on a single channel. The Hall wires used for commutation still go to their motor drivers. The other Hall taps are unused by this firmware. Share signal ground and use 3.3 V-compatible feedback on the ESP32. See the [wiring guide](../../models/motor-mount/wiring-guide.html#drive) for Hall conditioning and the ground-switch interface on the Rotation inputs. The ESP32 forward levels are left LOW and right HIGH.

Drive each Brake terminal through its ground-switch transistor. GPIO HIGH closes the switch to signal ground, and LOW opens it. Keep GPIO12 low at reset to preserve the ESP32's 3.3 V flash setting. [Espressif boot mode guide](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html)

Keep RX0/TX0 free for USB serial. Move the right Rotation control wire to D23 with motor power off and leave the old D2 connection unplugged. GPIO2 is a boot strap pin. [Espressif boot mode guide](https://docs.espressif.com/projects/esptool/en/latest/esp32/advanced-topics/boot-mode-selection.html)

## Connect

1. Build and upload from this folder with `pio run -t upload`.
2. Join **HotelBot** on your phone with password **hotelbot-drive**. Stay connected if the phone reports that the network has no internet.
3. Open **http://192.168.4.1/**. Only one Wi-Fi client can connect at a time. Opening a new controller tab takes over from the previous tab.

Drag the joystick forward or back to set travel speed. Drag left or right to steer. Horizontal motion alone pivots the car. The speed slider caps the wheel targets in RPM. The screen shows estimated wheel RPM first and target RPM second. Hall C measures speed magnitude. The first number uses the active direction output for its sign, so the displayed sign can change before the wheel physically reverses.

For a driving turn, move the stick forward or backward and add a small sideways input. The steering mix blends between the driving turn and the sideways pivot as the stick crosses between those regions.

Releasing the joystick cuts PWM, waits one PWM cycle if the wheel was powered, then engages that wheel's brake for up to 500 ms. A new command releases the brakes before drive resumes. Tap **Stop** to cut throttle and coast. The ESP32 coasts after 350 ms without a fresh motion command.

In normal drive, a reversal sets PWM to zero and switches the Rotation input on the same control update. A powered wheel gets one PWM cycle, about 2 ms, for the new duty to take effect before the switch. PID resumes on the next update. A wheel using Hall override keeps the 1.5 second pause.

The starting speed limit is 40 RPM, and the configured maximum is 240 RPM. The controller caps its internal PWM output at 55% during initial tuning. The limits are in [config.h](src/config.h).

For repeatable speed steps, enter each wheel's target under **Direct wheel speed** and press and hold **Hold to run**. Positive RPM is forward, negative RPM is reverse, and zero lets that wheel coast. Release the button to coast before entering a new setpoint. Direct RPM uses the configured wheel maximum, or the lower Hall override cap when active. The speed slider controls the joystick.

## Join eduroam

This profile is for University of Toronto UTORid accounts. The ESP32 joins the 2.4 GHz **eduroam** network using PEAP and checks the RADIUS server name and the [university's published certificate chain](https://wireless.utoronto.ca/ufaqs/#certificates). The existing **HotelBot** hotspot still serves the drive page at **http://192.168.4.1/**.

After flashing, stop the wheels and close other programs using the ESP32 serial port. Run the provisioning tool with the actual device path. It prompts locally for the UTORid and password and saves the profile in the ESP32's flash. Power-cycle the ESP32 after saving.

```sh
python3 tools/drive-bench/eduroam.py /dev/ttyUSB0 configure
python3 tools/drive-bench/eduroam.py /dev/ttyUSB0 status
python3 tools/drive-bench/eduroam.py /dev/ttyUSB0 forget
```

`status` prints the eduroam IP address when connected. Open that address with `http://` to reach the drive page from eduroam if the network allows traffic between clients. Anyone who can reach that address can operate the drive page without a login. `forget` removes the saved profile. Power-cycle after forgetting to end the active eduroam connection. The ESP32 uses a single radio, so the HotelBot hotspot follows eduroam's channel while connected.

## Update over Wi-Fi

Flash the OTA-enabled firmware over USB once. With the wheels stopped, provision a key over USB. The tool stores a private copy in `~/.config/hotelbot/ota.key`.

```sh
python3 tools/drive-bench/ota.py provision /dev/ttyUSB0
pio run -d firmware/drive-controller -e esp32dev
python3 tools/drive-bench/ota.py upload 100.67.9.22 firmware/drive-controller/.pio/build/esp32dev/firmware.bin
```

Use the actual serial port and HotelBot IP. The hotspot address `192.168.4.1` also works. The upload proves possession of the key without sending it over Wi-Fi. Drive stays disabled during an accepted upload. The ESP32 validates the image in the other app slot before selecting it and restarting. Wi-Fi credentials and saved wheel tuning remain in their existing flash storage. For USB recovery after an OTA update, use PlatformIO's full USB upload to restore the boot selection along with the application.

## Calibrate the Hall inputs

Raise both wheels and open a USB serial terminal at 115200 baud. Commands are lowercase and run when you press Enter. Keep the joystick released and the wheels free to turn by hand, with the Hall sensors powered.

1. Type `show` to get the current pulse counts.
2. Mark one wheel and rotate it by hand through five complete revolutions. Type `show` again. Its count should rise and the other wheel's count should stay steady.
3. Divide the count increase by five and round to the nearest integer. If it gained 75 pulses, for example, enter `ppr l 15` for the left wheel or `ppr r 15` for the right. Use the counts you actually measured.
4. Repeat for the other wheel, then type `save` to retain the calibration after a reboot. Motion stays disabled until both values are set.

If either wheel goes 1.5 seconds without Hall pulses while continuously receiving nonzero PWM, the controller cuts throttle on both wheels. When PID cuts PWM to zero and the wheel coasts, the Hall timeout is paused. Use `show` while turning the wheel by hand to check the sensor. Tap **Clear Hall fault** after fixing the cause.

When stopped on a Hall fault, tap **Drive with Hall override** to enable limited drive, then move the joystick. The target limit drops to 40 RPM and a wheel with a Hall fault uses open-loop PWM capped at 30%. Its actual speed is unknown. A wheel with healthy feedback keeps its PID loop. Release the joystick to coast. **Stop**, a timeout during motion, or opening a new controller session cancels the override. A newly detected Hall fault stops both wheels again. PWM faults still block drive.

If a hold stops unexpectedly, check the message above the RPM readings. It distinguishes a wheel's Hall fault, missing replies from the ESP32, an expired drive heartbeat, and an ESP32 restart. A restart reported by the ESP32 as a brownout appears as a voltage drop. During a hold, the browser sends a heartbeat every 75 ms over a persistent WebSocket when fewer than three commands are awaiting replies. The ESP32 cuts throttle after 350 ms without a fresh motion command, and recovery after a stop requires a new hold.

## Tune the speed loops

Tune with the wheels lifted first, then verify at low speed under load. Enter a repeatable step such as 30 RPM for one wheel and zero for the other. Select that wheel under **Live wheel graphs**. The graphs show the last 12 seconds of target and estimated RPM, signed tracking error, P/I/D contributions in PWM percentage points, and applied throttle as a percentage of full duty. Release the drive control before applying gains. Use **Apply left gains** or **Apply right gains** to test a change, and **Save applied gains** to retain it after a reboot. **Read gains** reloads the values currently in effect.

For USB tuning, enable `log on` for CSV output containing target RPM, measured RPM and internal PWM for each wheel, or use `log pid` for the PID plotter. Use `log off` to stop the log.

- Start with proportional control. The initial gains are `Kp=0.005`, `Ki=0.010`, and `Kd=0`. To tune proportional response first, set `pid l 0.005 0 0` and `pid r 0.005 0 0`. Change one wheel's gain at a time, with the stick released. Raise Kp in small steps until the wheel responds promptly. If it hunts or repeatedly overshoots, lower Kp.
- Add a small Ki for steady error. Try `pid l 0.005 0.005 0` with your chosen Kp substituted. Increase Ki gradually until the measured speed settles near the requested speed. Reduce Ki if recovery becomes slow or oscillatory. If PWM stays at its cap and RPM stays low, the wheel cannot reach that target under the current conditions.
- Leave Kd at zero initially. Derivative amplifies Hall timing noise and is usually unnecessary for wheel-speed control. Type `save` once both wheels track reliably. [WPILib PID tuning guide](https://docs.wpilib.org/en/2021/docs/software/advanced-control/introduction/tuning-pid-controller.html)

## USB bench program

[bench.py](../../tools/drive-bench/bench.py) can run speed steps and capture CSV traces from the computer connected to the ESP32. Install the dependencies from [requirements.txt](../../tools/drive-bench/requirements.txt) and close the serial monitor before opening the program. Use the actual ESP32 serial port.

```sh
python3 tools/drive-bench/bench.py /dev/ttyUSB0 status
python3 tools/drive-bench/bench.py /dev/ttyUSB0 step right --rpm 30 --ms 3000 --csv right-step.csv --wheels-lifted
python3 tools/drive-bench/bench.py /dev/ttyUSB0 pid right 0.005 0 0 --save
```

Lift and secure both wheels before a step. A step turns one wheel forward for at most four seconds at up to 40 RPM and 35% PWM. The Hall fault stop stays active, and a lost USB heartbeat stops the test after 350 ms. The phone joystick is locked during the step, and its **Stop** button can end the test.

### Plot target, estimated RPM and error

[plot.py](../../tools/drive-bench/plot.py) shows target and estimated RPM, signed tracking error, P/I/D contributions in PWM percentage points, and applied throttle as a percentage of full duty. Saved bench CSVs still show speed, error and throttle. PID term traces require the extended telemetry from `log pid`.

```sh
python3 tools/drive-bench/plot.py --port /dev/ttyUSB0 --window 10
python3 tools/drive-bench/plot.py --csv right-step.csv
```

Close any serial terminal or bench program using the same port before launching the live view. Release the joystick before changing gains. Enter Kp, Ki and Kd for either wheel and select **Apply**. Use **Read gains** to reload the controller's values and **Save applied** to persist them across a reboot. The plotter enables telemetry over USB and sends no motion commands.
