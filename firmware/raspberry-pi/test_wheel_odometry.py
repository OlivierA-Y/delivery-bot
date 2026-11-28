import json
import math
from pathlib import Path
from tempfile import TemporaryDirectory
import unittest

from wheel_odometry import EncoderOdometry, Geometry, load_geometry


def sample(stamp, left, right, *, boot="boot", session="session", faults=()):
    return {
        "sample_ms": stamp, "left_count": left, "right_count": right,
        "boot": boot, "session": session, "faults": list(faults),
    }


class EncoderOdometryTest(unittest.TestCase):
    def setUp(self):
        geometry = Geometry((0.005, 0.005), (360, 360), 0.4, (0.0, 0.0, 0.0), (0.0, 0.0, 0.0))
        self.odom = EncoderOdometry(geometry)
        self.assertIsNone(self.odom.update(sample(0, 10, -10)))

    def test_straight_and_reverse(self):
        self.assertEqual(self.odom.update(sample(40, 14, -6)), (0.5, 0.0))
        self.assertAlmostEqual(self.odom.x, 0.02)
        self.assertEqual(self.odom.update(sample(80, 10, -10)), (-0.5, 0.0))
        self.assertAlmostEqual(self.odom.x, 0.0)

    def test_left_pivot(self):
        linear, angular = self.odom.update(sample(40, 8, -8))
        self.assertEqual(linear, 0)
        self.assertAlmostEqual(angular, 1.25)
        self.assertAlmostEqual(self.odom.yaw, 0.05)
        self.assertEqual((self.odom.x, self.odom.y), (0.0, 0.0))

    def test_curve_uses_differential_drive_arc(self):
        self.odom.update(sample(40, 12, -4))
        self.assertAlmostEqual(self.odom.yaw, 0.05)
        self.assertAlmostEqual(self.odom.x, 0.4 * math.sin(0.05))
        self.assertAlmostEqual(self.odom.y, 0.4 * (1 - math.cos(0.05)))

    def test_duplicate_time_keeps_count_baseline(self):
        self.assertIsNone(self.odom.update(sample(0, 11, -9)))
        self.odom.update(sample(40, 14, -6))
        self.assertAlmostEqual(self.odom.x, 0.02)

    def test_restart_reconnect_and_jump_keep_pose_continuous(self):
        self.odom.update(sample(40, 14, -6))
        changes = [
            sample(0, 0, 0, boot="new"),
            sample(40, 5, 5, boot="new", session="reconnect"),
            sample(80, 400, 400, boot="new", session="reconnect"),
            sample(300, 410, 410, boot="new", session="reconnect"),
        ]
        for message in changes:
            self.assertIsNone(self.odom.update(message))
            self.assertAlmostEqual(self.odom.x, 0.02)
        self.odom.update(sample(340, 412, 412, boot="new", session="reconnect"))
        self.assertAlmostEqual(self.odom.x, 0.03)

    def test_fault_requires_fresh_baseline(self):
        self.assertIsNone(self.odom.update(sample(40, 14, -6, faults=["left_hall"])))
        self.assertIsNone(self.odom.update(sample(80, 16, -4)))
        self.odom.update(sample(120, 18, -2))
        self.assertAlmostEqual(self.odom.x, 0.01)


class GeometryTest(unittest.TestCase):
    def test_per_wheel_calibration_and_lidar_mount(self):
        config = {
            "left_wheel_radius_m": 0.0825, "right_wheel_radius_m": 0.076,
            "track_width_m": 0.46, "left_ticks_per_rev": 90, "right_ticks_per_rev": 120,
            "left_encoder_sign": -1, "max_plausible_wheel_rps": 5,
            "lidar": {"x_m": 0.12, "y_m": -0.04, "z_m": 0.08,
                      "roll_deg": 180, "pitch_deg": 2, "yaw_deg": 90},
        }
        with TemporaryDirectory() as directory:
            path = Path(directory) / "geometry.json"
            path.write_text(json.dumps(config))
            geometry = load_geometry(path)
        self.assertAlmostEqual(geometry.meters_per_tick[0], -math.tau * 0.0825 / 90)
        self.assertAlmostEqual(geometry.meters_per_tick[1], math.tau * 0.076 / 120)
        self.assertEqual(geometry.ticks_per_second_limit, (450, 600))
        self.assertEqual(geometry.track_width, 0.46)
        self.assertEqual(geometry.laser_xyz, (0.12, -0.04, 0.08))
        self.assertEqual(geometry.laser_rpy, (math.pi, math.radians(2), math.pi / 2))

    def test_template_requires_measurements(self):
        path = Path(__file__).parent / "slam" / "wheel_geometry.example.json"
        with self.assertRaisesRegex(ValueError, "left_wheel_radius_m"):
            load_geometry(path)


if __name__ == "__main__":
    unittest.main()
