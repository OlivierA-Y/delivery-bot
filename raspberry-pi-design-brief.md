# HotelBot Raspberry Pi software design brief

Build the Raspberry Pi side of HotelBot in this repository. The target is a supervised hackathon demo. A Raspberry Pi 4B should navigate to a pickup or delivery waypoint, ask a rider to press the elevator button when a trip changes floors, and resume navigation after the destination floor and exit are confirmed. Build and verify single-floor navigation before adding elevator motion.

## Hardware and current firmware

- Target Ubuntu Server 24.04 ARM64, ROS 2 Jazzy and Nav2. Use Python for the serial bridge and mission logic, and reuse the existing ROS 2 packages for mapping, localization and navigation. [ROS 2 Jazzy Ubuntu packages](https://repo.test.ros2.org/en/jazzy/Installation/Ubuntu-Install-Debs.html)
- The Pi handles navigation, chassis geometry, sensors, speech and mission decisions. The ESP32 owns PWM, direction control and the two wheel-speed loops for the ZS-X11H drivers. Use one USB data cable and one serial bridge process.
- The current [drive-controller firmware](firmware/drive-controller/README.md) runs on the classic ESP32 DevKit V1. It uses a Wi-Fi joystick, Hall C per wheel and USB bench commands. Its reported speed sign follows the direction output. The planned [Waveshare ESP32-S3-Zero architecture](models/motor-mount/system-diagram.html) uses three Hall channels per wheel for measured signed counts and a dedicated Pi drive protocol. Treat that protocol as a firmware dependency. Build against a mock first, require a successful handshake before sending real drive commands, and preserve the existing bench tools.
- Plan for a USB 2D lidar, with an RPLIDAR C1 as the candidate, plus a USB microphone and powered speaker. Make the lidar model, serial path, sensor transform and audio devices configurable. An optional BMP390 on the Pi’s I²C bus provides pressure-based altitude change as a secondary cue. Refer to the [Waveshare wiring schematic](models/motor-mount/full-wiring.html).

## Motion and odometry

Place `base_link` at the drive axle midpoint, with positive x forward and positive yaw counterclockwise. Measure the loaded radius of each wheel, effective track width, transitions per revolution for each Hall decoder, lidar pose and full chassis footprint relative to the axle. Calibrate track width with measured turns. The drive axle is offset from the chassis center, so a turn in place sweeps the overhang. Use the actual footprint for costmaps and clearance checks. A spin about a point ahead of or behind the axle is outside the fixed-wheel drivetrain’s instantaneous motion. Use a planned maneuver if the center of the chassis needs to end up near its starting point. [Differential-drive kinematics](https://control.ros.org/jazzy/doc/ros2_controllers/doc/mobile_robot_kinematics.html)

The bridge converts the final permitted body velocity into two wheel-speed targets. For axle separation `b`, body forward speed `v` and yaw rate `w`, the wheel linear speeds are `v - w*b/2` and `v + w*b/2`. Divide each by its own wheel radius and convert to the wire unit at the serial boundary. Scale both targets together when applying a wheel-speed limit so the requested curvature is preserved. Keep ROS interfaces in SI units.

Integrate signed cumulative Hall counts into `/odom` using per-wheel calibration and measured sample time. On a board reboot or counter discontinuity, establish a new count baseline without integrating the gap, keep pose continuous, mark feedback unavailable and require motion to be re-enabled. Publish `odom → base_link` from one owner. SLAM Toolbox owns `map → odom` during mapping and AMCL owns it during saved-map navigation. Include a static lidar transform. [ROS coordinate frames](https://github.com/ros-infrastructure/rep/blob/master/rep-0105.rst)

## Pi ↔ ESP32 contract

Open the [interactive USB protocol diagram](models/motor-mount/pi-esp32-protocol.html) for message-by-message handling and sample JSON.

Use bounded, versioned newline-delimited JSON over a configurable serial device. Prefer a stable USB device path when available. Keep the protocol and parser testable without ROS. The following is a proposed v1 contract for the mock and bridge. Confirm its fields and limits with the ESP32 implementation before a powered test.

| Message | Behavior |
| --- | --- |
| `hello` / `hello_ack` | Negotiate protocol version and supported commands. The Pi supplies a fresh session ID, and the reply echoes it and identifies the board boot. Start disarmed. |
| `arm`, `stop` | Explicitly enable motion once checks pass, or cancel active motion, command zero throttle and disarm. A stop, fault, stale feedback or reconnect requires a fresh arm. |
| `set_wheel_speeds` | Send signed `left_deg_s` and `right_deg_s` together. Start with a 25 Hz stream. Only a fresh permitted command can maintain speed-mode motion. |
| `move_wheels` | Send signed relative `left_deg` and `right_deg` plus a positive `max_deg_s` and a unique move ID. The ESP32 captures both count baselines once and coordinates both speed profiles. Report accepted, moving, done, canceled or fault. A duplicate move ID cannot launch another move. Gate this capability to supervised bench use in v1. |
| `keepalive` | Refresh the link watchdog during an accepted position move. Repeating a relative target could restart motion, so keepalive has its own message type. |
| `state` | Report board boot ID, monotonic sample time, signed cumulative counts, measured wheel speeds, armed state, active mode, last accepted sequence, move ID and result, and faults. |

Use a session identifier and increasing sequence numbers for accepted commands. Reject malformed, oversized, stale and out-of-range input. Make retries idempotent for one-shot commands. Start with a 250 ms firmware timeout as a value to agree and test on both sides. When input velocity expires, the Pi sends zero and stops refreshing nonzero targets. When feedback expires or the Pi loses the port, the Pi inhibits drive, the ESP32 watchdog cuts throttle, and recovery requires a fresh session if the link dropped. Record that the robot can still coast. Zero throttle and a reconnect never imply that motion may resume.

Create a single command path from Nav2 or supervised teleoperation through arbitration and Nav2 Collision Monitor to the bridge. Default to idle at boot. Inhibit drive when measurements needed for the selected mode are stale, and stop if the lidar or required transforms disappear during navigation. Ensure that the relative wheel-move command cannot bypass the navigation motion guard. Test disconnect and timeout behavior with the wheels raised and measure stopping distance before configuring the footprint’s stop and slowdown zones. [Nav2 Collision Monitor](https://docs.nav2.org/jazzy/configuration_and_development/configuration_guide/core_servers/collision_monitor/configuring_collision_monitor_node/)

## Navigation, speech and elevators

1. **Single floor.** Map the test floor with SLAM Toolbox, save the map, then use AMCL and Nav2 for waypoint navigation. Store waypoints and the map for each floor in configuration so room names and coordinates can change without code edits.
2. **Speech.** Provide local speech output and a small floor-number input grammar. Confirm which floor was heard. Provide an operator input fallback for noisy halls, ambiguous floor numbers and microphones that fail. Keep speech and recognition off the motion-control callbacks.
3. **Elevator lobby.** Configure a staging pose and door regions for each elevator bank. At the staging pose, stop and ask a rider to call the elevator. Track the door regions independently. Use lidar changes only where the sensor has a usable view. Ask the operator to select the car when the evidence is ambiguous, multiple doors open or the relevant door is outside the scan. An open door by itself does not establish that the threshold or cabin is clear.
4. **Board and ride.** Use supervised, low-speed entry when the chosen door and path are clear. Hold zero velocity while riding. Ask, “Which floor should I go to?” if the job has no confirmed destination. Otherwise ask the rider to press the known floor button. Verify the destination with the supervisor before exit. Pressure change can corroborate the floor change. Keep it advisory.
5. **Exit and resume.** Use a supervised, low-speed exit while the door and path are clear. Load the destination map and initialize localization from its configured elevator-exit pose. Resume Nav2 after pose validity is confirmed. On a timeout, blocked doorway, closing door or ambiguous floor, stop and wait for operator input.

Represent elevator handling as an explicit mission state machine with cancel, timeout and operator-confirmation events. Exercise the multi-door and floor-switch logic in a mock scenario. Use a controlled demo for any real elevator test. Keep the lidar, motion guard and operator supervision active during boarding and exit.

## Implementation order and acceptance

1. **Mock transport and odometry.** Add a ROS 2 workspace with a Python bridge, serial mock, configuration and launch files. Test equal wheel motion, opposite wheel motion, sign and units, reconnect, board reboot, invalid messages, stale commands and duplicate position-move IDs. Launch this without hardware.
2. **Bench integration.** Once firmware supports the agreed protocol, verify each wheel’s forward sign and measured counts by hand with drive disabled. With both wheels secured off the ground, verify speed commands, one paired move, stop, USB unplug, and restart. Confirm by measurement that the ESP32 cuts throttle on timeout and does not resume when the cable is reconnected.
3. **Single-floor demo.** Bring up the lidar, collect a map and demonstrate a waypoint run with the full footprint. Show that obstructing the path or dropping scan data inhibits motion. Include a teleop mode for commissioning.
4. **Supervised elevator demo.** Implement speech, mocked multi-car detection and floor changes before moving through a physical doorway. Demonstrate confirmation, cancel and recovery from ambiguous input. Use stationary speech and a simulated elevator ride as the demo fallback if motion through the threshold has not been validated.

Deliver source, launch files, configuration templates, the agreed wire protocol, automated mock tests and a short Pi setup and bench runbook. Keep port names, maps, audio devices, wheel measurements and motion limits configurable. Leave real motion disabled until those values are set and the ESP32 contract has been verified. Record measured values and completed hardware checks in the runbook.
