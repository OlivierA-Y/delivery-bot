# LD19 mapping on the Pi

This launch runs the LDROBOT LD19 driver, RF2O laser odometry, and SLAM Toolbox on ROS 2 Jazzy. The LiDAR driver publishes `/scan_raw`. A normalizer keeps `/scan` at 450 angular bins regardless of the number of readings in an LD19 sweep. RF2O publishes `/odom` and `odom → base_laser`. SLAM Toolbox publishes `/map` and `map → odom`.

The initial mapping frame is at the scanner. With motor drive disabled, position the scanner level and move the robot slowly by hand around the test area. Revisit some of the same walls to give SLAM Toolbox overlap for loop closure. The bench dashboard can stay open because the mapper uses the LiDAR port.

## Install and start

Configure the [ROS 2 Jazzy apt source](https://repo.test.ros2.org/en/jazzy/Installation/Ubuntu-Install-Debs.html) on Ubuntu Server 24.04 ARM64. Then install the build and runtime dependencies on the Pi.

```sh
sudo apt install ros-jazzy-ros-base ros-jazzy-slam-toolbox ros-jazzy-nav2-map-server \
  ros-jazzy-eigen3-cmake-module python3-colcon-common-extensions build-essential \
  cmake git libeigen3-dev libboost-dev
cd ~/hotelbot
bash firmware/raspberry-pi/slam/build_drivers.sh
```

From the Pi, identify the LD19 by physical USB port. On HotelBot it was the path ending in `1.3:1.0-port0`. The DevKit occupies the port ending in `1.2:1.0-port0`, and both adapters can share a USB serial number.

```sh
cd ~/hotelbot
source slam_ws/install/setup.bash
export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST
LIDAR_PORT=/dev/serial/by-path/platform-fd500000.pcie-pci-0000:01:00.0-usb-0:1.3:1.0-port0
ros2 launch firmware/raspberry-pi/slam/mapping.launch.py lidar_port:="$LIDAR_PORT"
```

Use one mapping launch at a time. Check the current commissioning session with `systemctl status hotelbot-slam` and stop it with `sudo systemctl stop hotelbot-slam` before starting a new map. The commissioning service is temporary. After a reboot, start mapping with the launch command above.

## View a live map remotely

With the Pi and workstation on the same Ethernet link, run the viewer from this repository on the workstation with RViz and ROS 2 installed.

```sh
firmware/raspberry-pi/slam/view_rviz.sh hotelbot-pi.local
```

RViz displays the occupancy grid, live LD19 returns in orange, and axes at the LiDAR pose. The viewer streams ROS topics over SSH and uses a local ROS domain. It defaults to the workstation's Lyrical install. Set `HOTELBOT_RVIZ_ROS_SETUP` to use a different local ROS setup file. Closing RViz ends the stream. The launcher disables Qt display scaling for RViz to work around flickering in the 3D view on scaled XWayland displays.

## Save a map

In another shell on the Pi, use the same ROS environment and discovery setting. Save the occupancy grid before stopping SLAM Toolbox. Pick a new filename for each survey.

```sh
cd ~/hotelbot
source slam_ws/install/setup.bash
export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST
mkdir -p maps
ros2 run nav2_map_server map_saver_cli -t /map -f "$PWD/maps/test-floor" \
  --ros-args -p save_map_timeout:=10.0
```

## Map with wheel odometry

The wheel odometry node reads signed Hall counts and sample times from the wheel dashboard’s loopback API. The dashboard keeps exclusive access to the ESP32. The node publishes `/odom` and the `odom → base_link` transform. It holds the last pose through a telemetry outage and starts from a fresh count baseline when the ESP32 reconnects, reports a Hall fault or returns an implausible count jump. SLAM Toolbox still uses the LD19 scans to correct the map.

Copy [the geometry template](wheel_geometry.example.json) to `~/hotelbot/wheel_geometry.json`. Enter both loaded wheel radii and the distance between wheel centers in metres. For a loaded radius, divide the distance rolled in one revolution by 2π. Each wheel’s ticks per revolution and sign are configurable. The calibrated DevKit uses 90 ticks per revolution and sign `1` on both wheels because the firmware already reports forward as positive. The maximum plausible wheel rate is a count-jump threshold and can be raised if encoder feedback is valid at higher speeds.

Enter the LiDAR offset from the axle midpoint in metres, with x forward, y left and z up. Set roll, pitch and yaw in degrees. Yaw is the counterclockwise angle from chassis-forward to the LiDAR’s scan x-axis. Keep the scanner level for 2D mapping. The launch uses the mount settings for the static `base_link → base_laser` transform.

Install the updated dashboard code and restart its service once to expose the encoder sample time. Save the current map if you want to keep it, then switch mapping modes.

```sh
cd ~/hotelbot
sudo systemctl restart hotelbot-wheel-bench
sudo systemctl stop hotelbot-slam
source slam_ws/install/setup.bash
export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST
LIDAR_PORT=/dev/serial/by-path/platform-fd500000.pcie-pci-0000:01:00.0-usb-0:1.3:1.0-port0
ros2 launch firmware/raspberry-pi/slam/wheel_mapping.launch.py \
  wheel_geometry:="$PWD/wheel_geometry.json" lidar_port:="$LIDAR_PORT"
```

With motor drive disabled, verify that a straight forward roll increases odometry x, that a left pivot increases yaw, and that returning to the same heading brings the scan into alignment with the map. Measure straight-line and turning errors before using this odometry for navigation. Use one mapping launch at a time so `/odom` and its transform have a single publisher.

## Bench results

The Pi received live 10 Hz LD19 scans, published both transforms and saved a 142 × 87 occupancy grid at 5 cm per cell to `~/hotelbot/maps/ld19-bringup.{pgm,yaml}`. The robot remained stationary for the live check. In a separate simulated scan sequence, RF2O estimated 0.625 m of forward travel for a 0.600 m translation, and SLAM Toolbox published the simulated map. A physical room survey is pending.
