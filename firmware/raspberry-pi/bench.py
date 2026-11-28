#!/usr/bin/env python3
"""Commission the HotelBot ESP32-S3 drive controller over native USB."""

import argparse
import json
import math
import re
import sys
from time import monotonic
from uuid import uuid4

import serial

COMMAND_LIMIT = 240
REPLY_LIMIT = 639   # The ESP32 formats replies in a 640-byte buffer, including NUL.
COMMAND_INTERVAL = 0.040
FEEDBACK_TIMEOUT = 0.150
REPLY_TIMEOUT = 0.150
TEST_SPEED_LIMIT = 120.0
LEGACY_MAX_WHEEL_DEG_S = 240.0
MIN_MOVE_SPEED = 10.0
TEST_ANGLE_LIMIT = 180.0
ID = re.compile(r"[A-Za-z0-9_-]{1,48}")
MOVE_STATES = ("accepted", "moving", "done", "canceled", "fault")


class BenchError(RuntimeError):
    pass


class CommandRejected(BenchError):
    def __init__(self, sequence, code):
        self.code = code
        super().__init__(f"ESP32 rejected sequence {sequence}: {code}")


class LinkLost(BenchError):
    pass


def identifier(value):
    return isinstance(value, str) and ID.fullmatch(value) is not None


def unsigned(value, bits):
    return type(value) is int and 0 <= value < 2**bits


def finite(value):
    return type(value) in (int, float) and abs(value) <= 1_000_000 and math.isfinite(value)


def unique_object(pairs):
    result = dict(pairs)
    if len(result) != len(pairs):
        raise ValueError("Duplicate JSON field")
    return result


def valid_state(state):
    counts = (state.get("left_count"), state.get("right_count"))
    sequence = state.get("last_seq")
    move_id = state.get("move_id")
    move_status = state.get("move_status")
    faults = state.get("faults")
    return (
        all(key in state for key in ("last_seq", "move_id", "move_status"))
        and unsigned(state.get("sample_ms"), 64)
        and all(type(count) is int and -(2**63) <= count < 2**63 for count in counts)
        and all(finite(state.get(key)) for key in ("left_deg_s", "right_deg_s"))
        and type(state.get("armed")) is bool
        and type(state.get("ready")) is bool
        and state.get("mode") in ("idle", "speed", "position")
        and (sequence is None or (unsigned(sequence, 32) and sequence > 0))
        and ((move_id is None and move_status is None)
             or (identifier(move_id) and move_status in MOVE_STATES))
        and isinstance(faults, list)
        and all(identifier(fault) for fault in faults)
    )


class SerialFrames:
    def __init__(self, port):
        self.port = port
        self.fragment = b""
        self.discard = False

    def send(self, **message):
        data = json.dumps(message, separators=(",", ":"), allow_nan=False).encode("ascii")
        if len(data) > COMMAND_LIMIT:
            raise BenchError("Command exceeds the firmware frame limit")
        try:
            if self.port.write(data + b"\n") != len(data) + 1:
                raise BenchError("Incomplete write to the ESP32")
        except serial.SerialTimeoutException as error:
            raise BenchError("ESP32 serial write timed out") from error
        except (serial.SerialException, OSError) as error:
            raise LinkLost(f"Serial write failed: {error}") from error

    def receive(self):
        try:
            data = self.port.read(max(1, min(self.port.in_waiting, 1024)))
        except (serial.SerialException, OSError) as error:
            raise LinkLost(f"Serial read failed: {error}") from error

        lines = (self.fragment + data).split(b"\n")
        self.fragment = lines.pop()
        for line in lines:
            discard, self.discard = self.discard, False
            if discard or not 0 < len(line) <= REPLY_LIMIT:
                continue
            try:
                message = json.loads(line.decode("ascii"), object_pairs_hook=unique_object)
            except (ValueError, RecursionError):
                continue
            if isinstance(message, dict):
                yield message

        if len(self.fragment) > REPLY_LIMIT or self.discard:
            self.fragment = b""
            self.discard = True


class Bench:
    def __init__(self, port):
        self.frames = SerialFrames(port)
        self.session = uuid4().hex
        self.sequence = 0
        self.boot = None
        self.capabilities = ()
        self.max_wheel_deg_s = LEGACY_MAX_WHEEL_DEG_S
        self.state = None
        self.last_sample_at = 0.0
        self.may_be_armed = False
        self.move_sequence = None

    def send(self, kind, **fields):
        self.sequence += 1
        self.frames.send(type=kind, session=self.session, seq=self.sequence, **fields)
        return self.sequence

    def poll(self):
        rejection = None
        for message in self.frames.receive():
            if message.get("session") != self.session:
                continue
            kind = message.get("type")
            boot = message.get("boot")
            if not identifier(boot):
                continue
            if self.boot is not None and boot != self.boot:
                raise BenchError("ESP32 boot ID changed. End this test and start a fresh session.")
            if kind == "hello_ack":
                if type(message.get("v")) is not int or message["v"] != 1:
                    raise BenchError("ESP32 did not negotiate protocol v1")
                capabilities = message.get("capabilities")
                if isinstance(capabilities, list) and all(identifier(item) for item in capabilities):
                    limit = message.get("max_wheel_deg_s", LEGACY_MAX_WHEEL_DEG_S)
                    if not finite(limit) or limit <= 0:
                        raise BenchError("ESP32 reported an invalid wheel speed limit")
                    self.boot = boot
                    self.capabilities = tuple(capabilities)
                    self.max_wheel_deg_s = limit
            elif kind == "error":
                sequence = message.get("seq")
                code = message.get("code")
                if not unsigned(sequence, 32) or not identifier(code) or sequence > self.sequence:
                    continue
                # A final keepalive can cross the move's self-disarm. Wait for its terminal state.
                if self.move_sequence is not None and sequence > self.move_sequence and code == "move_id":
                    continue
                # A disarm snapshot can follow the rejection in the same read.
                if rejection is None:
                    rejection = CommandRejected(sequence, code)
            elif kind == "state" and self.boot is not None and valid_state(message):
                previous = self.state
                if previous is not None:
                    if message["sample_ms"] < previous["sample_ms"]:
                        raise BenchError("ESP32 sample clock went backward")
                    if (message["last_seq"] or 0) < (previous["last_seq"] or 0):
                        continue
                if previous is None or message["sample_ms"] > previous["sample_ms"]:
                    self.last_sample_at = monotonic()
                self.state = message
        if rejection is not None:
            raise rejection

    def disarm_feedback(self):
        deadline = monotonic() + REPLY_TIMEOUT
        while monotonic() < deadline:
            state = self.state
            if (state is not None and not state["armed"] and state["mode"] == "idle"
                    and monotonic() - self.last_sample_at < FEEDBACK_TIMEOUT):
                self.may_be_armed = False
                return state
            try:
                self.poll()
            except CommandRejected as error:
                if error.code not in ("cannot_drive", "session"):
                    break
            except (BenchError, OSError):
                break
        return None

    def wait(self, predicate, timeout, reason):
        deadline = monotonic() + timeout
        while monotonic() < deadline:
            self.poll()
            if self.state is not None and predicate(self.state):
                self.check_fresh()
                return self.state
        raise BenchError(reason)

    def connect(self):
        deadline = monotonic() + 4
        next_hello = 0.0
        while monotonic() < deadline:
            now = monotonic()
            if self.boot is None and now >= next_hello:
                self.frames.send(type="hello", v=1, session=self.session)
                next_hello = now + 0.25
            self.poll()
            if self.state is not None:
                if self.state["armed"] or self.state["mode"] != "idle":
                    raise BenchError("Fresh session did not report idle and disarmed")
                return
        raise BenchError("No protocol v1 handshake and state. Check the port, USB cable and S3 firmware.")

    def check_fresh(self):
        if monotonic() - self.last_sample_at >= FEEDBACK_TIMEOUT:
            raise BenchError("ESP32 feedback went stale")

    def check_armed(self):
        self.check_fresh()
        if not self.state["armed"] or not self.state["ready"] or self.state["faults"]:
            raise BenchError(f"ESP32 drive became unavailable: {describe(self.state)}")

    def require_capability(self, capability):
        if capability not in self.capabilities:
            raise BenchError(f"ESP32 did not advertise {capability}")

    def arm(self):
        self.check_fresh()
        if not self.state["ready"] or self.state["faults"]:
            raise BenchError(f"ESP32 is not ready to arm: {describe(self.state)}")
        if any(abs(self.state[key]) > 8 for key in ("left_deg_s", "right_deg_s")):
            raise BenchError("Wait for both wheels to stop before arming")
        self.may_be_armed = True
        seq = self.send("arm")
        self.wait(lambda state: state["last_seq"] == seq and state["armed"],
                  REPLY_TIMEOUT, "ESP32 did not confirm arm")

    def stop(self):
        seq = self.send("stop")
        state = self.wait(
            lambda s: s["last_seq"] == seq and not s["armed"] and s["mode"] == "idle",
            REPLY_TIMEOUT, "ESP32 did not confirm stop",
        )
        self.may_be_armed = False
        return state

    def send_speed(self, left, right):
        return self.send("set_wheel_speeds", left_deg_s=left, right_deg_s=right)

    def speed_test(self, left, right, seconds, expect_disconnect=False):
        self.require_capability("set_wheel_speeds")
        self.arm()
        deadline = monotonic() + seconds
        next_send = 0.0
        first_sent = None
        confirmed = False
        try:
            while monotonic() < deadline:
                self.check_armed()
                now = monotonic()
                if now >= next_send:
                    seq = self.send_speed(left, right)
                    if first_sent is None:
                        first_sent = seq
                        ack_deadline = now + REPLY_TIMEOUT
                    next_send = now + COMMAND_INTERVAL
                self.poll()
                if (self.state["armed"] and self.state["mode"] == "speed"
                        and (self.state["last_seq"] or 0) >= first_sent):
                    if expect_disconnect and not confirmed:
                        print("Speed accepted. Unplug the ESP32 USB cable now.", flush=True)
                    confirmed = True
                if not confirmed and monotonic() >= ack_deadline:
                    raise BenchError("ESP32 did not accept the speed target")
        except LinkLost:
            if expect_disconnect and confirmed:
                print("USB loss observed during speed mode. Use status after reconnecting.", flush=True)
                return
            raise

        state = self.stop()
        if expect_disconnect:
            raise BenchError("USB was still connected when the test ended. Stop was confirmed.")
        if not confirmed:
            raise BenchError("ESP32 did not accept the speed target")
        print(f"Stop confirmed. {describe(state)}")

    def watchdog_test(self, left, right):
        self.require_capability("set_wheel_speeds")
        self.arm()
        sent = monotonic()
        seq = self.send_speed(left, right)
        self.wait(lambda s: s["last_seq"] == seq and s["armed"] and s["mode"] == "speed",
                  REPLY_TIMEOUT, "ESP32 did not accept the watchdog test target")
        state = self.wait(
            lambda s: (s["last_seq"] == seq and not s["armed"] and s["mode"] == "idle"
                       and "link_timeout" in s["faults"]),
            0.750, "ESP32 did not report a watchdog disarm",
        )
        self.may_be_armed = False
        elapsed = (monotonic() - sent) * 1000
        print(f"Watchdog disarm reported {elapsed:.0f} ms after the last command. {describe(state)}")

    def move_test(self, left, right, max_speed):
        self.require_capability("move_wheels")
        self.arm()
        move_id = uuid4().hex
        self.move_sequence = self.send(
            "move_wheels", move_id=move_id, left_deg=left, right_deg=right, max_deg_s=max_speed,
        )
        self.wait(
            lambda s: s["move_id"] == move_id and s["last_seq"] == self.move_sequence,
            REPLY_TIMEOUT, "ESP32 did not accept the paired move",
        )
        deadline = monotonic() + 20
        next_send = 0.0
        while monotonic() < deadline:
            self.check_fresh()
            state = self.state
            if state["move_id"] == move_id and state["move_status"] == "done":
                if state["armed"] or state["mode"] != "idle" or state["faults"]:
                    raise BenchError(f"Move completion did not leave drive idle: {describe(state)}")
                self.may_be_armed = False
                print(f"Paired move complete. {describe(state)}")
                return
            self.check_armed()
            if state["move_id"] != move_id or state["move_status"] not in ("accepted", "moving"):
                raise BenchError(f"Unexpected paired-move state: {describe(state)}")
            now = monotonic()
            if now >= next_send:
                self.send("keepalive", move_id=move_id)
                next_send = now + COMMAND_INTERVAL
            self.poll()
        raise BenchError("Paired move did not finish before the test deadline")

    def watch(self, seconds):
        deadline = monotonic() + seconds
        next_report = 0.0
        while monotonic() < deadline:
            self.poll()
            self.check_fresh()
            now = monotonic()
            if now >= next_report:
                print(describe(self.state), flush=True)
                next_report = now + 0.20


def describe(state):
    return (
        f"ms={state['sample_ms']} "
        f"left={state['left_count']:+d} ({state['left_deg_s']:+.1f} deg/s) "
        f"right={state['right_count']:+d} ({state['right_deg_s']:+.1f} deg/s) "
        f"armed={state['armed']} ready={state['ready']} mode={state['mode']} "
        f"faults={','.join(state['faults']) or 'none'}"
    )


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="ESP32-S3 USB port, preferably /dev/serial/by-id/…")
    parser.add_argument("--baud", type=int, default=115200)
    commands = parser.add_subparsers(dest="command", required=True)
    commands.add_parser("status", help="Open a disarmed session and print one state")
    watch = commands.add_parser("watch", help="Print signed counts while turning unpowered wheels by hand")
    watch.add_argument("--seconds", type=float, default=30)
    commands.add_parser("stop", help="Open a disarmed session and confirm an explicit stop")
    for name, help_text in (
        ("speed", "Stream a bounded wheel-speed test and confirm stop"),
        ("watchdog", "Send one speed target, then observe the board's watchdog disarm"),
        ("disconnect", "Stream briefly so the operator can unplug USB during speed mode"),
        ("move", "Run one paired relative move when the firmware advertises it"),
    ):
        command = commands.add_parser(name, help=help_text)
        units = "degrees" if name == "move" else "degrees per second"
        command.add_argument("--left", type=float, required=True, help=f"Signed left target in {units}")
        command.add_argument("--right", type=float, required=True, help=f"Signed right target in {units}")
        command.add_argument("--wheels-lifted", action="store_true", required=True,
                             help="Confirm both wheels are secured clear of the ground")
        if name in ("speed", "disconnect"):
            command.add_argument("--seconds", type=float, default=4 if name == "disconnect" else 1)
        if name == "move":
            command.add_argument("--max-deg-s", type=float, default=60)

    args = parser.parse_args(argv)
    if args.baud <= 0:
        parser.error("--baud must be positive")
    if args.command in ("watch", "speed", "disconnect"):
        minimum, maximum = (0.2, 600) if args.command == "watch" else (0.2, 4)
        if not math.isfinite(args.seconds) or not minimum <= args.seconds <= maximum:
            parser.error(f"--seconds must be within {minimum:g}–{maximum:g}")
    if hasattr(args, "left"):
        limit = TEST_ANGLE_LIMIT if args.command == "move" else TEST_SPEED_LIMIT
        targets = (args.left, args.right)
        if not all(math.isfinite(target) and abs(target) <= limit for target in targets) or not any(targets):
            parser.error(f"Targets must be finite, within ±{limit:g}, with at least one nonzero")
    if args.command == "move" and not MIN_MOVE_SPEED <= args.max_deg_s <= TEST_SPEED_LIMIT:
        parser.error(f"--max-deg-s must be within {MIN_MOVE_SPEED:g}–{TEST_SPEED_LIMIT:g}")
    return args


def serial_port(path, baud=115200):
    port = serial.Serial(baudrate=baud, timeout=0.008, write_timeout=0.060, exclusive=True)
    port.port = path
    # Assert the standard CDC control lines on open, including for USB-OTG CDC builds.
    port.dtr = port.rts = True
    return port


def main(argv=None):
    args = parse_args(argv)
    try:
        with serial_port(args.port, args.baud) as port:
            port.reset_input_buffer()
            bench = Bench(port)
            try:
                bench.connect()
                print(f"v1 session={bench.session} boot={bench.boot} "
                      f"capabilities={','.join(bench.capabilities) or 'none'}", flush=True)
                if args.command == "status":
                    print(describe(bench.state))
                elif args.command == "watch":
                    bench.watch(args.seconds)
                elif args.command == "stop":
                    print(f"Stop confirmed. {describe(bench.stop())}")
                elif args.command == "watchdog":
                    bench.watchdog_test(args.left, args.right)
                elif args.command == "move":
                    bench.move_test(args.left, args.right, args.max_deg_s)
                else:
                    bench.speed_test(args.left, args.right, args.seconds, args.command == "disconnect")
            finally:
                if bench.may_be_armed:
                    try:
                        bench.send("stop")
                    except (BenchError, OSError):
                        pass  # The ESP32 watchdog still applies when the port is gone.
    except KeyboardInterrupt:
        return 130
    except (BenchError, OSError, serial.SerialException) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
