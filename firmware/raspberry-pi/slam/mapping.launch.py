from pathlib import Path
import sys

from launch import LaunchDescription
from launch.actions import DeclareLaunchArgument, EmitEvent, ExecuteProcess, RegisterEventHandler
from launch.conditions import IfCondition
from launch.events import matches_action
from launch.substitutions import LaunchConfiguration
from launch_ros.actions import LifecycleNode, Node
from launch_ros.event_handlers import OnStateTransition
from launch_ros.events.lifecycle import ChangeState
from lifecycle_msgs.msg import Transition


def generate_launch_description():
    here = Path(__file__).resolve().parent
    lidar_port = LaunchConfiguration("lidar_port")
    base_frame = LaunchConfiguration("base_frame")
    mapper = LifecycleNode(
        package="slam_toolbox",
        executable="async_slam_toolbox_node",
        name="slam_toolbox",
        namespace="",
        parameters=[str(here / "mapper.yaml"), {
            "base_frame": base_frame, "use_sim_time": False, "use_lifecycle_manager": False,
        }],
        output="screen",
    )
    activate = RegisterEventHandler(OnStateTransition(
        target_lifecycle_node=mapper,
        start_state="configuring",
        goal_state="inactive",
        entities=[EmitEvent(event=ChangeState(
            lifecycle_node_matcher=matches_action(mapper),
            transition_id=Transition.TRANSITION_ACTIVATE,
        ))],
    ))
    return LaunchDescription([
        DeclareLaunchArgument("lidar_port", default_value="", description="LD19 physical USB serial path"),
        DeclareLaunchArgument("launch_lidar", default_value="true"),
        DeclareLaunchArgument("laser_odometry", default_value="true"),
        DeclareLaunchArgument("base_frame", default_value="base_laser"),
        Node(
            package="ldlidar_stl_ros2",
            executable="ldlidar_stl_ros2_node",
            name="hotelbot_ld19",
            condition=IfCondition(LaunchConfiguration("launch_lidar")),
            parameters=[{
                "product_name": "LDLiDAR_LD19", "topic_name": "scan_raw",
                "frame_id": "base_laser", "port_name": lidar_port, "port_baudrate": 230400,
                "laser_scan_dir": True, "enable_angle_crop_func": False,
            }],
            output="screen",
        ),
        ExecuteProcess(
            cmd=[sys.executable, str(here / "scan_normalizer.py")],
            name="scan_normalizer",
            output="screen",
        ),
        Node(
            package="rf2o_laser_odometry",
            executable="rf2o_laser_odometry_node",
            name="hotelbot_laser_odometry",
            condition=IfCondition(LaunchConfiguration("laser_odometry")),
            parameters=[{
                "laser_scan_topic": "/scan", "odom_topic": "/odom",
                "publish_tf": True, "base_frame_id": base_frame, "odom_frame_id": "odom",
                "init_pose_from_topic": "", "freq": 12.0,
            }],
            arguments=["--ros-args", "--log-level", "error"],
            output="screen",
        ),
        activate,
        mapper,
        EmitEvent(event=ChangeState(
            lifecycle_node_matcher=matches_action(mapper),
            transition_id=Transition.TRANSITION_CONFIGURE,
        )),
    ])
