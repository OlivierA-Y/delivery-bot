from pathlib import Path
import sys

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, ExecuteProcess, IncludeLaunchDescription, Shutdown
from launch.launch_description_sources import PythonLaunchDescriptionSource
from launch.substitutions import LaunchConfiguration


def generate_launch_description():
    here = Path(__file__).resolve().parent
    return LaunchDescription([
        DeclareLaunchArgument("wheel_geometry", description="Path to calibrated wheel geometry JSON"),
        DeclareLaunchArgument("lidar_port", description="LD19 physical USB serial path"),
        DeclareLaunchArgument("http_port", default_value="8719", description="Local wheel dashboard port"),
        ExecuteProcess(
            cmd=[sys.executable, str(here.parent / "wheel_odometry.py"),
                 "--geometry", LaunchConfiguration("wheel_geometry"),
                 "--http-port", LaunchConfiguration("http_port")],
            name="wheel_odometry",
            output="screen",
            on_exit=[Shutdown(reason="Wheel odometry process exited.")],
        ),
        IncludeLaunchDescription(
            PythonLaunchDescriptionSource(str(here / "mapping.launch.py")),
            launch_arguments={
                "lidar_port": LaunchConfiguration("lidar_port"),
                "base_frame": "base_link",
                "laser_odometry": "false",
            }.items(),
        ),
    ])
