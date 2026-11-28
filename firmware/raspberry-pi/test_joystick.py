from contextlib import nullcontext
from types import SimpleNamespace
import unittest
from unittest.mock import patch

from bench import Bench, BenchError, CommandRejected
from bench_web import WheelBench, joystick_targets


def stopped_state(session, boot):
    return {
        "type": "state", "session": session, "boot": boot,
        "sample_ms": 500, "last_seq": 304, "armed": False, "ready": False,
        "mode": "idle", "faults": ["right_hall"], "left_count": 22, "right_count": 20,
        "left_deg_s": 0, "right_deg_s": 0, "move_id": None, "move_status": None,
    }


class JoystickTests(unittest.TestCase):
    def setUp(self):
        self.now = 10.0
        clock = patch("bench_web.monotonic", lambda: self.now)
        clock.start()
        self.addCleanup(clock.stop)
        self.bench = WheelBench("unused", 115200)
        self.bench.can_drive = True
        self.bench.state = {"ready": True, "armed": False, "mode": "idle", "faults": []}
        self.bench.sample_at = self.now

    def start_drive(self):
        return self.bench.reserve_drive({"epoch": self.bench.epoch, "clear": True})["run"]

    def test_continuous_joystick_stops_on_signal_loss(self):
        run = self.start_drive()
        self.bench.max_drive_rpm = 200
        for index in range(101):
            self.now = 10 + index * .05
            self.bench.hold({"run": run, "x": 0, "y": 1, "rpm": 200})
        self.assertEqual((self.bench.run.left, self.bench.run.right), (1200, 1200))
        self.bench.hold({"run": run, "x": -1, "y": 0, "rpm": 120})
        self.assertEqual((self.bench.run.left, self.bench.run.right), (-720, 720))
        self.now += .301
        with self.assertRaises(BenchError):
            self.bench.hold({"run": run, "x": 1, "y": 0, "rpm": 200})
        self.assertTrue(self.bench.stop_requested)
        self.assertIsNone(self.bench.run)

    def test_invalid_active_update_stops_drive(self):
        run = self.start_drive()
        with self.assertRaisesRegex(ValueError, "axes"):
            self.bench.hold({"run": run, "x": float("nan"), "y": 0, "rpm": 180})
        self.assertTrue(self.bench.stop_requested)
        self.assertIsNone(self.bench.run)

    def test_independent_bench_limit(self):
        run = self.bench.reserve({"epoch": self.bench.epoch, "lifted": True, "left": 60, "right": 60})["run"]
        self.bench.hold({"run": run})
        for index in range(1, 20):
            self.now = 10 + index * .2
            self.bench.hold({"run": run})
        self.now = 14
        with self.assertRaises(BenchError):
            self.bench.hold({"run": run})
        self.assertTrue(self.bench.stop_requested)

    def test_simple_mixer_and_legacy_cap(self):
        diagonal = {"x": 0.75, "y": 0.75, "rpm": 200}
        self.assertEqual(joystick_targets(diagonal, 200), (1200, 0))
        self.assertEqual(joystick_targets(diagonal, 40), (240, 0))
        self.assertEqual(joystick_targets({"x": 0.5, "y": 0, "rpm": 60}, 200), (180, -180))
        self.assertEqual(joystick_targets({"x": 0, "y": 1, "rpm": 0}, 200), (0, 0))
        with self.assertRaisesRegex(ValueError, "200 RPM"):
            joystick_targets({**diagonal, "rpm": 201}, 200)

    def test_speed_limit_negotiation(self):
        bench = Bench(None)
        hello = {
            "type": "hello_ack", "v": 1, "session": bench.session, "boot": "test",
            "capabilities": ["set_wheel_speeds"],
        }
        bench.frames = SimpleNamespace(receive=lambda: [hello])
        bench.poll()
        self.assertEqual(bench.max_wheel_deg_s, 240)
        hello["max_wheel_deg_s"] = 1200
        bench.poll()
        self.assertEqual(bench.max_wheel_deg_s, 1200)
        hello["max_wheel_deg_s"] = -1
        with self.assertRaises(BenchError):
            bench.poll()

    def test_drive_rejection_preserves_adjacent_fault_snapshot(self):
        bench = Bench(None)
        bench.boot = "test"
        bench.sequence = 305
        error = {"type": "error", "session": bench.session, "boot": bench.boot,
                 "seq": 305, "code": "cannot_drive"}
        state = stopped_state(bench.session, bench.boot)
        bench.frames = SimpleNamespace(receive=lambda: [error, state])
        with self.assertRaises(CommandRejected):
            bench.poll()
        self.assertEqual(bench.disarm_feedback()["faults"], ["right_hall"])

    def test_reconnect_retains_fault_and_requires_new_hold(self):
        run = self.start_drive()
        failed = SimpleNamespace(
            boot="test", capabilities=("set_wheel_speeds",), max_wheel_deg_s=1200,
            may_be_armed=True, connect=lambda: None, send=lambda *_: None,
        )

        def disarm_feedback():
            failed.may_be_armed = False
            return stopped_state("test", "test")

        failed.disarm_feedback = disarm_feedback
        recovered = SimpleNamespace(
            boot="test", capabilities=("set_wheel_speeds",), max_wheel_deg_s=1200,
            may_be_armed=False, connect=lambda: None,
        )
        port = SimpleNamespace(reset_input_buffer=lambda: None)

        def control(bench):
            if bench is failed:
                raise CommandRejected(305, "cannot_drive")
            self.bench.closed.set()

        with (
            patch("bench_web.serial_port", side_effect=lambda *_: nullcontext(port)),
            patch("bench_web.Bench", side_effect=(failed, recovered)),
            patch.object(self.bench, "control", side_effect=control),
            patch.object(self.bench.closed, "wait"),
            patch("builtins.print") as log,
        ):
            self.bench.work()
        self.assertIsNone(self.bench.run)
        self.assertIn("right_hall", self.bench.status()["last_drive_error"])
        self.assertIn("right_hall", log.call_args.args[0])
        with self.assertRaises(BenchError):
            self.bench.hold({"run": run, "x": 0, "y": 1, "rpm": 15})


if __name__ == "__main__":
    unittest.main()
