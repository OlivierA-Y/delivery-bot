#!/usr/bin/env python3
"""Relay mapping topics from the Pi to a local RViz session over SSH."""

import argparse
from functools import partial
from pathlib import Path
import signal
import struct
import subprocess
import sys

from nav_msgs.msg import OccupancyGrid
import rclpy
from rclpy.executors import ExternalShutdownException
from rclpy.qos import DurabilityPolicy, QoSProfile, qos_profile_sensor_data
from rclpy.signals import SignalHandlerOptions
from sensor_msgs.msg import LaserScan
from tf2_msgs.msg import TFMessage


LATCHED = QoSProfile(depth=1, durability=DurabilityPolicy.TRANSIENT_LOCAL)
STATIC_TF = QoSProfile(depth=32, durability=DurabilityPolicy.TRANSIENT_LOCAL)
TOPICS = (
    ("/map", OccupancyGrid, LATCHED),
    ("/scan", LaserScan, qos_profile_sensor_data),
    ("/tf", TFMessage, QoSProfile(depth=50)),
    ("/tf_static", TFMessage, STATIC_TF),
)
HEADER = struct.Struct("!BI")
MAX_MESSAGE_SIZE = 64 * 1024 * 1024
REMOTE_COMMAND = (
    "bash -c 'source /opt/ros/jazzy/setup.bash && "
    "export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST && "
    "exec /usr/bin/python3 -u - send'"
)


def send(node):
    output = sys.stdout.buffer

    def write_message(index, payload):
        output.write(HEADER.pack(index, len(payload)))
        output.write(payload)
        output.flush()

    for index, (topic, message_type, qos) in enumerate(TOPICS):
        node.create_subscription(message_type, topic, partial(write_message, index), qos, raw=True)
    rclpy.spin(node)


def receive(node, host, ssh_options):
    publishers = [node.create_publisher(message_type, topic, qos) for topic, message_type, qos in TOPICS]
    command = [
        "ssh", "-T", "-o", "BatchMode=yes", "-o", "ConnectTimeout=8",
        "-o", "ServerAliveInterval=5", "-o", "ServerAliveCountMax=3",
        *ssh_options, "--", host, REMOTE_COMMAND,
    ]
    with Path(__file__).open("rb") as source, subprocess.Popen(
        command, stdin=source, stdout=subprocess.PIPE,
    ) as process:
        try:
            while header := process.stdout.read(HEADER.size):
                if len(header) != HEADER.size:
                    raise ValueError("Incomplete message header from the Pi")
                index, length = HEADER.unpack(header)
                if not 0 <= index < len(publishers) or not 0 < length <= MAX_MESSAGE_SIZE:
                    raise ValueError("Invalid message header from the Pi")
                payload = process.stdout.read(length)
                if len(payload) != length:
                    raise ValueError("Incomplete ROS message from the Pi")
                publishers[index].publish(payload)
            raise OSError(f"SSH stream from {host} ended (exit status {process.wait()})")
        finally:
            if process.poll() is None:
                process.terminate()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    commands = parser.add_subparsers(dest="mode", required=True)
    commands.add_parser("send")
    client = commands.add_parser("receive")
    client.add_argument("host")
    client.add_argument("ssh_options", nargs=argparse.REMAINDER)
    arguments = parser.parse_args()

    signal.signal(signal.SIGTERM, signal.default_int_handler)
    rclpy.init(args=[], signal_handler_options=SignalHandlerOptions.NO)
    node = rclpy.create_node(f"hotelbot_rviz_{arguments.mode}")
    try:
        if arguments.mode == "send":
            send(node)
        else:
            receive(node, arguments.host, arguments.ssh_options)
    except (KeyboardInterrupt, ExternalShutdownException, BrokenPipeError):
        return 0
    except (OSError, ValueError) as error:
        print(f"RViz stream: {error}", file=sys.stderr)
        return 1
    finally:
        node.destroy_node()
        rclpy.try_shutdown()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
