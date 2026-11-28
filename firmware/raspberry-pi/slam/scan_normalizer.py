#!/usr/bin/env python3
"""Resample the rotating LD19's variable-size scans onto a fixed angular grid."""

import math

import rclpy
from rclpy.executors import ExternalShutdownException
from rclpy.node import Node
from rclpy.qos import qos_profile_sensor_data
from sensor_msgs.msg import LaserScan


def resample(scan, bin_count, limit):
    increment = math.tau / bin_count
    first_angle = (increment - math.tau) / 2
    maximum = min(limit, scan.range_max)
    if not math.isfinite(scan.angle_increment) or scan.angle_increment <= 0:
        return None
    if not math.isfinite(scan.angle_min) or not 0 < scan.range_min < maximum:
        return None
    ranges = [math.inf] * bin_count
    for index, distance in enumerate(scan.ranges):
        if not scan.range_min <= distance <= maximum:
            continue
        angle = scan.angle_min + index * scan.angle_increment
        slot = round((angle - first_angle) / increment) % bin_count
        ranges[slot] = min(distance, ranges[slot])
    if not any(math.isfinite(distance) for distance in ranges):
        return None
    # The vendor stamps each assembled sweep as a unit. Keep that stamp and
    # leave time_increment zero because the circular rebinning changes its origin.
    return LaserScan(
        header=scan.header,
        angle_min=first_angle,
        angle_max=first_angle + increment * (bin_count - 1),
        angle_increment=increment,
        time_increment=0.0,
        scan_time=scan.scan_time,
        range_min=scan.range_min,
        range_max=maximum,
        ranges=ranges,
    )


class ScanNormalizer(Node):
    def __init__(self):
        super().__init__("ld19_scan_normalizer")
        self.output = self.create_publisher(LaserScan, "/scan", qos_profile_sensor_data)
        self.raw = self.create_subscription(
            LaserScan, "/scan_raw", self.normalize, qos_profile_sensor_data,
        )

    def normalize(self, scan):
        adjusted = resample(scan, bin_count=450, limit=12.0)
        if adjusted is not None:
            self.output.publish(adjusted)


def main():
    rclpy.init()
    node = ScanNormalizer()
    try:
        rclpy.spin(node)
    except (KeyboardInterrupt, ExternalShutdownException):
        pass
    finally:
        node.destroy_node()
        rclpy.try_shutdown()


if __name__ == "__main__":
    main()
