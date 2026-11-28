#!/usr/bin/env python3
"""Publish wheel odometry using the Pi dashboard's read-only encoder feedback."""

import argparse
from dataclasses import dataclass
import json
import math
from pathlib import Path
from time import monotonic
from urllib.request import urlopen

from geometry_msgs.msg import TransformStamped
from nav_msgs.msg import Odometry
import rclpy
from rclpy.duration import Duration
from rclpy.executors import ExternalShutdownException
from rclpy.node import Node
from tf2_ros import StaticTransformBroadcaster, TransformBroadcaster

from bench import FEEDBACK_TIMEOUT, finite, identifier, valid_state


ENCODER_FAULTS = {"uncalibrated", "left_hall", "right_hall"}
POLL_INTERVAL = 0.04
HTTP_TIMEOUT = 0.07


@dataclass(frozen=True)
class Geometry:
    meters_per_tick: tuple[float, float]
    ticks_per_second_limit: tuple[float, float]
    track_width: float
    laser_xyz: tuple[float, float, float]
    laser_rpy: tuple[float, float, float]


def number(config, key, *, positive=False):
    value = config.get(key)
    if not finite(value) or (positive and value <= 0):
        requirement = "a positive number" if positive else "a number"
        raise ValueError(f"Set {key} to {requirement}.")
    return float(value)


def load_geometry(path):
    with Path(path).expanduser().open() as config_file:
        config = json.load(config_file)
    if not isinstance(config, dict) or not isinstance(config.get("lidar"), dict):
        raise ValueError("Expected wheel geometry with a lidar object.")

    meters_per_tick = []
    ticks_per_second_limit = []
    config.setdefault("max_plausible_wheel_rps", 4)
    max_rps = number(config, "max_plausible_wheel_rps", positive=True)
    for wheel in ("left", "right"):
        radius = number(config, f"{wheel}_wheel_radius_m", positive=True)
        key = f"{wheel}_ticks_per_rev"
        ticks = config.get(key)
        if type(ticks) is not int or not 6 <= ticks <= 10000:
            raise ValueError(f"Set {key} to an integer from 6 to 10000.")
        key = f"{wheel}_encoder_sign"
        sign = config.get(key, 1)
        if type(sign) is not int or sign not in (-1, 1):
            raise ValueError(f"Set {key} to 1 or -1.")
        meters_per_tick.append(sign * math.tau * radius / ticks)
        ticks_per_second_limit.append(ticks * max_rps)
    lidar = {"z_m": 0, "roll_deg": 0, "pitch_deg": 0, **config["lidar"]}
    return Geometry(
        tuple(meters_per_tick),
        tuple(ticks_per_second_limit),
        number(config, "track_width_m", positive=True),
        tuple(number(lidar, coordinate) for coordinate in ("x_m", "y_m", "z_m")),
        tuple(math.radians(number(lidar, angle)) for angle in ("roll_deg", "pitch_deg", "yaw_deg")),
    )


class EncoderOdometry:
    def __init__(self, geometry):
        self.geometry = geometry
        self.x = self.y = self.yaw = 0.0
        self.previous = None
        self.unavailable = "Waiting for a second encoder sample."

    def rebaseline(self, reason):
        self.previous = None
        self.unavailable = reason

    def update(self, state):
        if ENCODER_FAULTS.intersection(state["faults"]):
            self.rebaseline("ESP32 reported an encoder fault.")
            return None

        previous = self.previous
        if (previous is None or state["boot"] != previous["boot"]
                or state["session"] != previous["session"]):
            self.previous = state
            self.unavailable = "Waiting for a second encoder sample in this ESP32 session."
            return None

        dt = (state["sample_ms"] - previous["sample_ms"]) / 1000
        if dt == 0:
            return None
        self.previous = state
        if not 0 < dt < FEEDBACK_TIMEOUT:
            self.unavailable = "Encoder clock or sample gap changed. Reset the count baseline."
            return None

        deltas = tuple(state[key] - previous[key] for key in ("left_count", "right_count"))
        limits = self.geometry.ticks_per_second_limit
        if any(abs(delta) > math.ceil(limit * dt) + 1 for delta, limit in zip(deltas, limits)):
            self.unavailable = "Implausible encoder count change. Reset the count baseline."
            return None

        left, right = (delta * scale for delta, scale in zip(deltas, self.geometry.meters_per_tick))
        distance = (left + right) / 2
        turn = (right - left) / self.geometry.track_width
        half_turn = turn / 2
        chord = distance * math.sin(half_turn) / half_turn if half_turn else distance
        self.x += chord * math.cos(self.yaw + half_turn)
        self.y += chord * math.sin(self.yaw + half_turn)
        self.yaw = math.remainder(self.yaw + turn, math.tau)
        self.unavailable = None
        return distance / dt, turn / dt


def set_rotation(quaternion, roll, pitch, yaw):
    sr, sp, sy = (math.sin(angle / 2) for angle in (roll, pitch, yaw))
    cr, cp, cy = (math.cos(angle / 2) for angle in (roll, pitch, yaw))
    quaternion.x = sr * cp * cy - cr * sp * sy
    quaternion.y = cr * sp * cy + sr * cp * sy
    quaternion.z = cr * cp * sy - sr * sp * cy
    quaternion.w = cr * cp * cy + sr * sp * sy


def covariance(diagonal):
    matrix = [0.0] * 36
    matrix[::7] = diagonal
    return matrix


class WheelOdometry(Node):
    def __init__(self, geometry, http_port):
        super().__init__("hotelbot_wheel_odometry")
        self.url = f"http://127.0.0.1:{http_port}/api/status"
        self.odometry = EncoderOdometry(geometry)
        self.reported = None
        self.publisher = self.create_publisher(Odometry, "/odom", 10)
        self.broadcaster = TransformBroadcaster(self)
        self.static_broadcaster = StaticTransformBroadcaster(self)

        mount = TransformStamped()
        mount.header.stamp = self.get_clock().now().to_msg()
        mount.header.frame_id = "base_link"
        mount.child_frame_id = "base_laser"
        translation = mount.transform.translation
        translation.x, translation.y, translation.z = geometry.laser_xyz
        set_rotation(mount.transform.rotation, *geometry.laser_rpy)
        self.static_broadcaster.sendTransform(mount)
        self.timer = self.create_timer(POLL_INTERVAL, self.poll)

    def read_feedback(self):
        with urlopen(self.url, timeout=HTTP_TIMEOUT) as response:
            status = json.load(response)
        if not isinstance(status, dict):
            raise ValueError("Invalid dashboard response.")
        state, stamp = status.get("state"), status.get("sample_monotonic")
        if (not isinstance(state, dict) or not valid_state(state)
                or not identifier(state.get("boot")) or not identifier(state.get("session"))
                or type(stamp) not in (int, float) or not math.isfinite(stamp)
                or not 0 <= monotonic() - stamp < FEEDBACK_TIMEOUT):
            raise ValueError("Encoder feedback is missing, invalid or stale.")
        return state, stamp

    def report(self, reason):
        if reason == self.reported:
            return
        self.reported = reason
        if reason is None:
            self.get_logger().info("Publishing wheel odometry.")
        else:
            self.get_logger().warning(reason)

    def poll(self):
        try:
            state, sample_time = self.read_feedback()
        except (OSError, ValueError) as error:
            self.odometry.rebaseline(f"Waiting for encoder feedback. {error}")
            self.report(self.odometry.unavailable)
            return
        velocity = self.odometry.update(state)
        self.report(self.odometry.unavailable)
        if velocity is None:
            return
        stamp = self.get_clock().now() - Duration(seconds=monotonic() - sample_time)
        self.publish(stamp.to_msg(), *velocity)

    def publish(self, stamp, linear, angular):
        odom = Odometry()
        odom.header.stamp = stamp
        odom.header.frame_id = "odom"
        odom.child_frame_id = "base_link"
        pose = odom.pose.pose
        pose.position.x = self.odometry.x
        pose.position.y = self.odometry.y
        set_rotation(pose.orientation, 0.0, 0.0, self.odometry.yaw)
        odom.twist.twist.linear.x = linear
        odom.twist.twist.angular.z = angular
        # Nominal commissioning variances. Measure these before using them for sensor fusion.
        odom.pose.covariance = covariance((0.0025, 0.0025, 1e6, 1e6, 1e6, 0.01))
        odom.twist.covariance = covariance((0.0225, 1e6, 1e6, 1e6, 1e6, 0.09))

        transform = TransformStamped()
        transform.header = odom.header
        transform.child_frame_id = odom.child_frame_id
        transform.transform.translation.x = pose.position.x
        transform.transform.translation.y = pose.position.y
        transform.transform.rotation = pose.orientation
        self.broadcaster.sendTransform(transform)
        self.publisher.publish(odom)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--geometry", type=Path, required=True)
    parser.add_argument("--http-port", type=int, default=8719)
    args = parser.parse_args()
    if not 1 <= args.http_port <= 65535:
        parser.error("HTTP port must be between 1 and 65535.")
    try:
        geometry = load_geometry(args.geometry)
    except (OSError, ValueError) as error:
        parser.error(str(error))

    rclpy.init(args=[])
    node = WheelOdometry(geometry, args.http_port)
    try:
        rclpy.spin(node)
    except (KeyboardInterrupt, ExternalShutdownException):
        pass
    finally:
        node.destroy_node()
        rclpy.try_shutdown()


if __name__ == "__main__":
    main()
