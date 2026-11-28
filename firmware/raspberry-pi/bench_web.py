#!/usr/bin/env python3
"""Serve the HotelBot drive controls on the Pi's loopback interface."""

import argparse
from dataclasses import dataclass
from http import HTTPStatus
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
import json
from pathlib import Path
import signal
from threading import Event, Lock, Thread
from time import monotonic
from uuid import uuid4

from bench import (
    Bench, BenchError, COMMAND_INTERVAL, CommandRejected, FEEDBACK_TIMEOUT, LEGACY_MAX_WHEEL_DEG_S,
    TEST_SPEED_LIMIT, describe, finite, identifier, serial_port,
)

KEEPALIVE_SECONDS = 0.30
RUN_SECONDS = 4.0
MAX_DRIVE_RPM = 200
STATIC = Path(__file__).with_suffix("")
ASSETS = {
    "/": ("index.html", "text/html; charset=utf-8"),
    "/bench.css": ("bench.css", "text/css; charset=utf-8"),
    "/bench.js": ("bench.js", "text/javascript; charset=utf-8"),
    "/drive": ("drive.html", "text/html; charset=utf-8"),
    "/drive.css": ("drive.css", "text/css; charset=utf-8"),
    "/drive.js": ("drive.js", "text/javascript; charset=utf-8"),
}


@dataclass
class Run:
    key: str
    left: float
    right: float
    expires: float
    joystick: bool = False
    started_at: float | None = None


def joystick_targets(body, max_rpm):
    x, y, throttle = body.get("x"), body.get("y"), body.get("rpm")
    if not all(finite(value) and abs(value) <= 1 for value in (x, y)):
        raise ValueError("Joystick axes must be between -1 and 1.")
    if not finite(throttle) or not 0 <= throttle <= MAX_DRIVE_RPM:
        raise ValueError(f"Throttle must be between 0 and {MAX_DRIVE_RPM} RPM.")

    left, right = y + x, y - x
    peak = max(1, abs(left), abs(right))
    limit = 6 * min(throttle, max_rpm)
    return left / peak * limit, right / peak * limit


class WheelBench:
    def __init__(self, path, baud):
        self.path = path
        self.baud = baud
        self.lock = Lock()
        self.closed = Event()
        self.epoch = uuid4().hex
        self.run = None
        self.stop_requested = False
        self.state = None
        self.sample_at = 0.0
        self.boot = None
        self.can_drive = False
        self.max_drive_rpm = LEGACY_MAX_WHEEL_DEG_S / 6
        self.message = "Connecting to the ESP32."
        self.last_drive_error = None
        self.thread = Thread(target=self.work, name="wheel-serial", daemon=True)

    def _cancel(self, message):
        self.run = None
        self.epoch = uuid4().hex
        self.stop_requested = True
        self.message = message

    def _expire(self, now):
        if self.run is not None and now >= self.run.expires:
            message = ("Drive signal expired. Release and press again to drive." if self.run.joystick
                       else "Hold ended. Release and press again to run.")
            self._cancel(message)

    def _fresh(self, now):
        return self.state is not None and now - self.sample_at < FEEDBACK_TIMEOUT

    def _can_reserve(self, now):
        return (self.run is None and not self.stop_requested and self._fresh(now)
                and self.can_drive and self.state["ready"] and not self.state["armed"]
                and self.state["mode"] == "idle" and not self.state["faults"])

    def status(self):
        with self.lock:
            now = monotonic()
            self._expire(now)
            state = self.state if self._fresh(now) else None
            elapsed = 0
            if self.run is not None and not self.run.joystick and self.run.started_at is not None:
                elapsed = min(RUN_SECONDS, now - self.run.started_at)
            return {
                "epoch": self.epoch, "state": state, "boot": self.boot,
                "sample_monotonic": self.sample_at if state is not None else None,
                "available": self._can_reserve(now),
                "run": self.run.key if self.run is not None else None,
                "target": [self.run.left, self.run.right] if state and state["mode"] == "speed"
                          and self.run is not None else [0, 0],
                "elapsed": elapsed, "limit": RUN_SECONDS, "max_drive_rpm": self.max_drive_rpm,
                "message": self.message, "last_drive_error": self.last_drive_error,
            }

    def reserve(self, body):
        left, right = body.get("left"), body.get("right")
        if body.get("lifted") is not True:
            raise ValueError("Confirm both wheels are secured off the ground.")
        if not all(finite(value) and abs(value) <= TEST_SPEED_LIMIT for value in (left, right)):
            raise ValueError(f"Both targets must be within ±{TEST_SPEED_LIMIT:g} °/s.")
        if left == 0 and right == 0:
            raise ValueError("Enter a nonzero target for at least one wheel.")
        return self._reserve(body.get("epoch"), left, right)

    def reserve_drive(self, body):
        if body.get("clear") is not True:
            raise ValueError("Confirm the robot is in view and the area is clear.")
        return self._reserve(body.get("epoch"), 0, 0, joystick=True)

    def _reserve(self, epoch, left, right, *, joystick=False):
        with self.lock:
            now = monotonic()
            self._expire(now)
            if epoch != self.epoch or not self._can_reserve(now):
                raise BenchError("Drive is busy or unavailable. Check the controller status.")
            self.run = Run(uuid4().hex, left, right, now + 0.75, joystick=joystick)
            self.epoch = uuid4().hex
            self.last_drive_error = None
            self.message = "Waiting for hold confirmation."
            return {"run": self.run.key}

    def hold(self, body):
        with self.lock:
            now = monotonic()
            self._expire(now)
            run = self.run
            if run is None or body.get("run") != run.key:
                raise BenchError("Hold ended. Release and press again.")
            if run.joystick:
                try:
                    run.left, run.right = joystick_targets(body, self.max_drive_rpm)
                except ValueError:
                    self._cancel("Invalid joystick update. Drive stopped.")
                    raise
            if run.started_at is None:
                run.started_at = now
                self.message = "Hold active."
            run.expires = now + KEEPALIVE_SECONDS
            if not run.joystick:
                run.expires = min(run.expires, run.started_at + RUN_SECONDS)
        return {"ok": True}

    def request_stop(self, run=None):
        if run is not None and not identifier(run):
            raise ValueError("Invalid hold identifier.")
        with self.lock:
            if run is None or (self.run is not None and run == self.run.key):
                self._cancel("Stop requested.")
        return {"ok": True}

    def work(self):
        while not self.closed.is_set():
            bench = None
            try:
                with serial_port(self.path, self.baud) as port:
                    port.reset_input_buffer()
                    bench = Bench(port)
                    try:
                        bench.connect()
                        with self.lock:
                            self.boot = bench.boot
                            self.can_drive = "set_wheel_speeds" in bench.capabilities
                            self.max_drive_rpm = min(MAX_DRIVE_RPM, bench.max_wheel_deg_s / 6)
                            self.message = "ESP32 connected."
                        self.control(bench)
                    except CommandRejected as error:
                        if error.code in ("cannot_drive", "session") and bench.may_be_armed:
                            try:
                                bench.send("stop")
                            except (BenchError, OSError):
                                pass
                            feedback = bench.disarm_feedback()
                            if feedback is not None and feedback["faults"]:
                                raise BenchError(f"ESP32 stopped drive: {describe(feedback)}") from error
                        raise
                    finally:
                        if bench.may_be_armed:
                            try:
                                bench.send("stop")
                            except (BenchError, OSError):
                                pass
            except (BenchError, OSError) as error:
                message = str(error)
                with self.lock:
                    interrupted_drive = self.run is not None or bool(bench and bench.may_be_armed)
                    if interrupted_drive:
                        self.last_drive_error = message
                    log_error = interrupted_drive or message != self.message
                    self.state = None
                    self._cancel(message)
                if log_error:
                    print(f"Drive controller: {message}", flush=True)
                self.closed.wait(0.75)

    def control(self, bench):
        driving = None
        next_send = 0
        while not self.closed.is_set():
            bench.poll()
            bench.check_fresh()
            with self.lock:
                self.state, self.sample_at = bench.state, bench.last_sample_at
                self._expire(monotonic())
                run = self.run
                stop = self.stop_requested or (driving is not None and run is not driving)
            if stop:
                state = bench.stop()
                driving = None
                with self.lock:
                    self.state, self.sample_at = state, bench.last_sample_at
                    self.stop_requested = False
                    if self.message == "Stop requested.":
                        self.message = "ESP32 confirmed stop."
                continue
            if run is None or run.started_at is None:
                continue
            if driving is None:
                bench.arm()
                driving, next_send = run, 0
            bench.check_armed()
            with self.lock:
                now = monotonic()
                self._expire(now)
                if now >= next_send and self.run is driving and not self.stop_requested:
                    bench.send_speed(driving.left, driving.right)
                    next_send = now + COMMAND_INTERVAL


class Handler(BaseHTTPRequestHandler):
    def setup(self):
        self.request.settimeout(2)
        super().setup()

    def log_message(self, *_args):
        pass

    def reply(self, status, data, content_type="application/json"):
        self.send_response(status)
        self.send_header("Content-Type", content_type)
        self.send_header("Content-Length", str(len(data)))
        self.send_header("Cache-Control", "no-store")
        self.send_header("X-Content-Type-Options", "nosniff")
        self.send_header("Referrer-Policy", "no-referrer")
        self.send_header("Content-Security-Policy",
                         "default-src 'self'; object-src 'none'; frame-ancestors 'none'; base-uri 'none'")
        self.end_headers()
        try:
            self.wfile.write(data)
        except OSError:
            pass

    def json_reply(self, data, status=HTTPStatus.OK):
        self.reply(status, json.dumps(data, allow_nan=False).encode())

    def allowed_host(self):
        port = self.server.server_port
        return self.headers.get("Host") in (f"localhost:{port}", f"127.0.0.1:{port}")

    def do_GET(self):
        if not self.allowed_host():
            self.json_reply({"error": "Use the loopback address."}, HTTPStatus.FORBIDDEN)
        elif self.path == "/api/status":
            self.json_reply(self.server.bench.status())
        elif self.path in ASSETS:
            filename, kind = ASSETS[self.path]
            self.reply(HTTPStatus.OK, (STATIC / filename).read_bytes(), kind)
        else:
            self.json_reply({"error": "Unknown route."}, HTTPStatus.NOT_FOUND)

    def do_POST(self):
        if not self.allowed_host() or self.headers.get("Origin") != f"http://{self.headers.get('Host')}":
            self.json_reply({"error": "Request origin rejected."}, HTTPStatus.FORBIDDEN)
            return
        bench = self.server.bench
        try:
            length = int(self.headers.get("Content-Length", 0))
            if self.headers.get_content_type() != "application/json" or not 0 < length <= 512:
                raise ValueError("Expected a small JSON request.")
            body = json.loads(self.rfile.read(length))
            if not isinstance(body, dict):
                raise ValueError("Expected an object.")
            if self.path == "/api/start":
                result = bench.reserve(body)
            elif self.path == "/api/drive/start":
                result = bench.reserve_drive(body)
            elif self.path == "/api/hold":
                result = bench.hold(body)
            elif self.path == "/api/stop":
                result = bench.request_stop(body.get("run"))
            else:
                self.json_reply({"error": "Unknown route."}, HTTPStatus.NOT_FOUND)
                return
            self.json_reply(result)
        except (ValueError, RecursionError, TimeoutError) as error:
            self.json_reply({"error": str(error)}, HTTPStatus.BAD_REQUEST)
        except BenchError as error:
            self.json_reply({"error": str(error)}, HTTPStatus.CONFLICT)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="Physical path to the ESP32 serial port")
    parser.add_argument("--baud", type=int, default=115200)
    parser.add_argument("--http-port", type=int, default=8719)
    args = parser.parse_args()
    bench = WheelBench(args.port, args.baud)
    finished = Event()
    with ThreadingHTTPServer(("127.0.0.1", args.http_port), Handler) as server:
        server.bench = bench
        server.timeout = 0.20
        signal.signal(signal.SIGTERM, lambda *_: finished.set())
        signal.signal(signal.SIGINT, lambda *_: finished.set())
        bench.thread.start()
        print(f"Wheel bench at http://127.0.0.1:{server.server_port}", flush=True)
        try:
            while not finished.is_set():
                server.handle_request()
        finally:
            bench.closed.set()
            bench.thread.join(timeout=5)


if __name__ == "__main__":
    main()
