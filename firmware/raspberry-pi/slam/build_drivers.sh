#!/usr/bin/env bash
# Build pinned LD19 and RF2O driver sources against the installed ROS 2 Jazzy.
set -eo pipefail

workspace=${1:-"$HOME/hotelbot/slam_ws"}
mkdir -p "$workspace/src"
workspace=$(cd -- "$workspace" && pwd)
source /opt/ros/jazzy/setup.bash

checkout() {
    local name=$1 url=$2 revision=$3
    local path="$workspace/src/$name"
    local current
    if [[ ! -d "$path/.git" ]]; then
        git init -q "$path"
        git -C "$path" remote add origin "$url"
    fi
    current=$(git -C "$path" rev-parse --verify HEAD 2>/dev/null || true)
    if [[ -z "$current" ]]; then
        git -C "$path" fetch --depth 1 origin "$revision"
        git -C "$path" checkout -q --detach FETCH_HEAD
    elif [[ "$current" != "$revision" ]]; then
        printf 'Unexpected revision in %s. Choose a fresh workspace.\n' "$path" >&2
        return 1
    fi
}

checkout ldlidar_stl_ros2 https://github.com/ldrobotSensorTeam/ldlidar_stl_ros2.git bf668a89baf722a787dadc442860dcbf33a82f5a
checkout rf2o_laser_odometry https://github.com/MAPIRlab/rf2o_laser_odometry.git b38c68e46387b98845ecbfeb6660292f967a00d3

# Ubuntu 24.04 does not bring in pthread declarations through the vendor logger's other headers.
logger="$workspace/src/ldlidar_stl_ros2/ldlidar_driver/src/logger/log_module.cpp"
if ! grep -q '^#include <pthread.h>$' "$logger"; then
    sed -i '1i#include <pthread.h>' "$logger"
fi
sed -i 's/int serial_port_baudrate;/int serial_port_baudrate = 230400;/' \
    "$workspace/src/ldlidar_stl_ros2/src/demo.cpp"

# Start RF2O at the identity quaternion when no initial pose topic is configured.
sed -i 's/initial_robot_pose.pose.pose.orientation.w = 0;/initial_robot_pose.pose.pose.orientation.w = 1;/' \
    "$workspace/src/rf2o_laser_odometry/src/CLaserOdometry2DNode.cpp"

# RF2O installs only its executable, which already compiles the solver.
sed -i 's/^add_library(${PROJECT_NAME} src\/CLaserOdometry2D.cpp)$/add_library(${PROJECT_NAME} EXCLUDE_FROM_ALL src\/CLaserOdometry2D.cpp)/' \
    "$workspace/src/rf2o_laser_odometry/CMakeLists.txt"

cd "$workspace"
jobs=${HOTELBOT_BUILD_JOBS:-1}
export CMAKE_BUILD_PARALLEL_LEVEL="$jobs"
export MAKEFLAGS="-j$jobs"
colcon build --executor sequential --packages-select ldlidar_stl_ros2 rf2o_laser_odometry \
    --cmake-args -DCMAKE_BUILD_TYPE=Release -DCMAKE_CXX_STANDARD=17 -DBUILD_TESTING=OFF
