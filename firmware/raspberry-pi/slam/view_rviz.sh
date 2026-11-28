#!/usr/bin/env bash
set -eo pipefail

here=$(cd -- "$(dirname -- "$0")" && pwd)
pi=${1:-hotelbot-pi}
if (( $# > 0 )); then shift; fi

source "${HOTELBOT_RVIZ_ROS_SETUP:-/opt/ros/lyrical/setup.bash}"
export ROS_DOMAIN_ID="${HOTELBOT_RVIZ_DOMAIN_ID:-83}"
export ROS_AUTOMATIC_DISCOVERY_RANGE=LOCALHOST
export QT_QPA_PLATFORM="${QT_QPA_PLATFORM:-xcb}"
export LIBGL_ALWAYS_SOFTWARE="${LIBGL_ALWAYS_SOFTWARE:-1}"

# RViz's Ogre viewport can flicker after resizing on scaled XWayland displays.
unset QT_SCREEN_SCALE_FACTORS QT_DEVICE_PIXEL_RATIO
export QT_ENABLE_HIGHDPI_SCALING=0 QT_AUTO_SCREEN_SCALE_FACTOR=0 QT_SCALE_FACTOR=1

/usr/bin/python3 "$here/rviz_stream.py" receive "$pi" "$@" &
relay=$!
rviz2 -d "$here/hotelbot_mapping.rviz" &
viewer=$!

cleanup() {
    kill "$relay" "$viewer" 2>/dev/null || true
    if kill -0 "$relay" 2>/dev/null || kill -0 "$viewer" 2>/dev/null; then
        sleep 1
        kill -KILL "$relay" "$viewer" 2>/dev/null || true
    fi
    wait "$relay" "$viewer" 2>/dev/null || true
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM
wait -n "$relay" "$viewer"
