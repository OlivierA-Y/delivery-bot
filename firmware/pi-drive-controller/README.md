# HotelBot Pi drive controller

Pi drive firmware for the **ESP32 DevKit V1** and the **Waveshare ESP32-S3-Zero**. It uses the Pi's USB serial link, runs the two wheel-speed loops, and reports signed wheel motion from all three Hall channels on each wheel. The DevKit Wi-Fi controller and its bench tools are separate firmware. [Pi design brief](../../raspberry-pi-design-brief.md)

## Build and check

Run these commands from the repository root.

```sh
pio run -d firmware/pi-drive-controller -e esp32dev
pio run -d firmware/pi-drive-controller -e waveshare_esp32_s3_zero
bash firmware/pi-drive-controller/test/host/run.sh
```

The host tests use a C++17 compiler and cJSON. Install the host's `libcjson-dev` package or use the cJSON headers from the installed PlatformIO ESP32 framework and the Linux cJSON runtime.

The DevKit uses its USB-to-UART bridge, typically `/dev/ttyUSB*`. The S3 uses native USB, typically `/dev/ttyACM*`. Use 115200 and a stable `/dev/serial/by-id/` path when available. Keep the Pi bridge as the only process reading and writing this port. The DevKit detects a cable disconnect through the 250 ms command watchdog because the UART bridge does not report USB link state to the ESP32. [Espressif USB CDC setup](https://docs.espressif.com/projects/arduino-esp32/en/latest/tutorials/cdc_dfu_flash.html)

| Signal | DevKit left | DevKit right | S3 left | S3 right |
| --- | ---: | ---: | ---: | ---: |
| PWM | 15 | 16 | 4 | 5 |
| Rotation via ground switch | 23 | 4 | 6 | 7 |
| Hall A via 3.3 V buffer | 19 | 17 | 8 | 9 |
| Hall B via 3.3 V buffer | 21 | 5 | 10 | 12 |
| Hall C via 3.3 V buffer | 22 | 18 | 11 | 13 |

The DevKit uses 490 Hz PWM and the S3 uses 2 kHz. All three Hall inputs must be connected on each wheel. See the wiring guides for signal reference, buffers, ground switches, and the default-off PWM checks. [DevKit V1 wiring guide](../../models/motor-mount/devkit-v1-wiring.html) · [S3 wiring guide](../../models/motor-mount/wiring-guide.html#drive)

The Hall speed estimate keeps the previous period for the first two intervals that are more than 25% shorter than expected. Three consecutive short intervals confirm a speed increase. Every valid transition still contributes to the signed counts for odometry.

## Serial v1

Send one flat JSON object per LF-terminated line. CRLF is accepted. The input limit is 240 bytes before LF. Use plain ASCII and IDs of 1–48 letters, digits, underscores, or hyphens. Malformed frames are discarded. Every command after `hello` carries the current `session` and a positive, increasing 32-bit integer `seq`. Rejected frames do not extend the watchdog.

```json
{"type":"hello","v":1,"session":"demo-1"}
{"type":"arm","session":"demo-1","seq":1}
{"type":"set_wheel_speeds","session":"demo-1","seq":2,"left_deg_s":90,"right_deg_s":110}
{"type":"stop","session":"demo-1","seq":3}
```

Wait for the corresponding reply or fresh state after each step. Use a fresh session ID on each connection. A valid new `hello` halts and disarms the controller. A retry of the same live `hello` repeats the acknowledgement without changing motion or refreshing the watchdog. An unsupported version disarms the controller. After disconnect or watchdog expiry, the previous session ID is rejected. A wheel fault latches while armed and can be cleared through a fresh handshake once the underlying cause is gone. Every recovery requires a new explicit `arm`.

| Message | Additional fields and behavior |
| --- | --- |
| `hello_ack` | `v`, echoed `session`, per-boot `boot` ID, and a list of `capabilities`. |
| `arm` | Requires calibrated wheels, healthy hardware, and measured speeds at most 8 °/s in magnitude. A confirming `state` shows `armed: true` and the accepted sequence. Start sending the permitted speed commands within the watchdog window. |
| `set_wheel_speeds` | `left_deg_s`, `right_deg_s`. Positive means chassis forward. Both targets must fit the `max_wheel_deg_s` field from `hello_ack`. The current build reports 1200 °/s (200 RPM). For earlier boards without this field, use 240 °/s (40 RPM). Send at 25 Hz. Zero targets coast. |
| `move_wheels` | Bench-only capability, disabled in the default build. Requires an armed, stationary controller in idle mode. Send a unique `move_id`, `left_deg`, `right_deg` (up to ±720° each), and `max_deg_s` (positive and at most 240 °/s). Captures each Hall baseline once, caps each wheel in proportion to its planned travel, and slows near each target. |
| `keepalive` | Refreshes the watchdog only for the active position `move_id`. It never starts a move or extends speed mode. |
| `stop` | Cancels the move, cuts PWM, and disarms. The same stop can be retried until re-armed. |
| `state` | Sent every 40 ms after handshake. Fields are `session`, `boot`, `sample_ms`, signed cumulative `left_count` and `right_count`, measured `left_deg_s` and `right_deg_s`, `armed`, `ready`, `mode`, `last_seq`, `move_id`, `move_status`, and an array of `faults`. Nullable fields start at `null`. |
| `error` | Reports the input `session`, `seq` (0 for hello), `boot`, and a machine-readable `code` when a parsed message is rejected. |

Position mode retains up to 24 accepted move IDs per session. A repeated ID reports its stored status with a state snapshot while leaving the current sequence, watchdog, and move targets alone. Status values are `accepted`, `moving`, `done`, `canceled`, and `fault`. A move completes after both wheels settle within one Hall transition of their targets for 100 ms and then disarms. It faults and disarms after 20 seconds.

Five continuous seconds of wheel PWM with no change in signed encoder count triggers `left_stall` or `right_stall` and cuts output on both wheels. A Hall transition or a return to zero PWM resets the stall timer.

The Hall decoder filters brief GPIO glitches, with a stability window of up to 100 µs based on the wheel calibration. An invalid Hall sequence that passes that filter reports `left_hall` or `right_hall` and cuts output. A wheel coasting opposite to the command waits at zero PWM. Measured opposite motion while PWM is applied cuts output to both wheels and reports `left_direction` or `right_direction`. A 250 ms command gap, USB loss, or PWM failure also cuts output. The wheels can coast after throttle goes to zero.

## Commissioning

1. With motor power disconnected, flash the target for your board and verify the handshake and disarmed status. The default build reports `uncalibrated` and refuses to arm.
2. With the wheels raised and the motors unpowered, turn each by hand several revolutions. Record the change in its count, divide by the number of turns, and determine which sign corresponds to chassis-forward motion. Use a low-duty commissioning test to determine each Rotation output polarity.
3. Set each wheel's `transitionsPerRevolution`, `hallForwardSign`, and `forwardLevel` in [drive_config.h](src/drive_config.h), then rebuild. Confirm that forward hand turns produce positive counts on both wheels. Adjust the wheel gains only during bench commissioning.
4. With the wheels still raised, test low-speed forward and reverse, stop, missed commands, USB disconnect, and reconnect. Measure the time to zero PWM and ensure reconnect requires a fresh handshake and arm. Enable `kEnableBenchMoves` only for the supervised paired-move test.
5. Record the actual counts per revolution, polarity, speed response, and stopping distance before using the Pi navigation stack.

Both firmware builds and the host tests have been run. USB communication, Hall polarity, PWM, and stopping behavior on hardware remain to be measured.
