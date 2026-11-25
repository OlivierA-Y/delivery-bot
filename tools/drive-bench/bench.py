#!/usr/bin/env python3
"""Run a bounded one-wheel RPM test and capture the ESP32's serial telemetry."""

import argparse
import csv
import re
import sys
from pathlib import Path
from time import monotonic

import serial


FIELDS = ("ms", "l_target_rpm", "l_measured_rpm", "l_pwm",
          "r_target_rpm", "r_measured_rpm", "r_pwm")
SAMPLE = re.compile(r"\d+,-?\d+\.\d,-?\d+\.\d,\d+,-?\d+\.\d,-?\d+\.\d,\d+")
STATUS = re.compile(r"BENCH STATUS ready=([01]) fault=(\d+) active=([01]) log=([01])")
GAIN_RANGES = "Kp 0 to 1, Ki 0 to 2, Kd −1 to 1"


def valid_gains(kp, ki, kd):
    return 0 <= kp <= 1 and 0 <= ki <= 2 and -1 <= kd <= 1


class DriveBench:
    def __init__(self, port):
        self.port = port
        self.partial = b""

    def send(self, command):
        self.port.write((command + "\n").encode("ascii"))

    def lines(self):
        *complete, self.partial = (self.partial + self.port.read(1024)).split(b"\n")
        if len(self.partial) > 4096:
            self.partial = b""
        return [line.decode("ascii", errors="replace").strip() for line in complete]

    def status(self):
        deadline = monotonic() + 3
        next_query = 0
        while monotonic() < deadline:
            if monotonic() >= next_query:
                self.send("bench status")
                next_query = monotonic() + 0.4
            for line in self.lines():
                if match := STATUS.fullmatch(line):
                    return tuple(int(value) for value in match.groups())
        raise RuntimeError("No BENCH STATUS response. Check the serial port and firmware version.")

    def tune(self, command, expected_prefix):
        _, _, active, _ = self.status()
        if active:
            raise RuntimeError("Wait for the wheel step to finish.")
        self.send(command)
        deadline = monotonic() + 2
        while monotonic() < deadline:
            for line in self.lines():
                if line.startswith(expected_prefix):
                    return line
                if line.startswith(("Release the joystick", "Invalid command", "Could not save")):
                    raise RuntimeError(line)
        raise RuntimeError("The firmware did not confirm the tuning command.")

    def step(self, side, rpm, duration_ms, writer):
        ready, fault, active, logging = self.status()
        if not ready or fault or active:
            raise RuntimeError(f"Drive unavailable (ready={ready}, fault={fault}, active={active}).")

        writer.writerow(FIELDS)
        token = None
        result = None
        next_keepalive = 0
        samples = 0
        self.send("log on")
        try:
            # Some USB adapters reset the ESP32 on open. Wait for quiet telemetry.
            deadline = monotonic() + 3
            quiet = 0
            while quiet < 5 and monotonic() < deadline:
                for line in self.lines():
                    if SAMPLE.fullmatch(line):
                        row = line.split(",")
                        writer.writerow(row)
                        idle = all(abs(float(row[index])) < 3 for index in (2, 5)) and all(
                            float(row[index]) == 0 for index in (1, 3, 4, 6)
                        )
                        quiet = quiet + 1 if idle else 0
            if quiet < 5:
                raise RuntimeError("The wheels did not report a steady idle state.")
            deadline = monotonic() + duration_ms / 1000 + 3
            self.send(f"bench start {side} {rpm} {duration_ms}")
            while monotonic() < deadline:
                if token is not None and result is None and monotonic() >= next_keepalive:
                    self.send(f"bench keep {token}")
                    next_keepalive = monotonic() + 0.09
                for line in self.lines():
                    if line.startswith("BENCH START "):
                        token = int(line.rsplit(" ", 1)[1])
                    elif line.startswith("BENCH REJECT "):
                        raise RuntimeError(line)
                    elif line.startswith("BENCH END "):
                        result = line.rsplit(" ", 1)[1]
                        deadline = monotonic() + 0.85
                    elif SAMPLE.fullmatch(line):
                        writer.writerow(line.split(","))
                        samples += 1
        finally:
            try:
                if result is None:
                    self.send("bench stop")
                if not logging:
                    self.send("log off")
            except (OSError, serial.SerialException):
                # Firmware independently stops on lost keepalives.
                pass

        if result != "done":
            raise RuntimeError(f"Step ended with {result or 'no confirmation'}. {samples} samples captured.")
        return samples


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="USB serial device, for example /dev/ttyUSB0")
    actions = parser.add_subparsers(dest="action", required=True)
    actions.add_parser("status", help="Check firmware, drive readiness and Hall fault status")
    pid = actions.add_parser("pid", help="Set the gains for one stationary wheel")
    pid.add_argument("wheel", choices=("left", "right"))
    pid.add_argument("kp", type=float)
    pid.add_argument("ki", type=float)
    pid.add_argument("kd", type=float)
    pid.add_argument("--save", action="store_true", help="Persist gains before closing this connection")
    actions.add_parser("save", help="Save the current gains and PPR to the ESP32")
    step = actions.add_parser("step", help="Run and record a forward speed step on one wheel")
    step.add_argument("wheel", choices=("left", "right"))
    step.add_argument("--rpm", type=int, default=30, metavar="10–40")
    step.add_argument("--ms", type=int, default=3000, metavar="500–4000")
    step.add_argument("--csv", type=Path, required=True, help="New CSV file for the step trace")
    step.add_argument("--wheels-lifted", action="store_true", help="Confirm both wheels are clear of the ground")
    args = parser.parse_args()
    if args.action == "step" and not args.wheels_lifted:
        parser.error("Lift and secure both wheels, then pass --wheels-lifted.")
    if args.action == "step" and (not 10 <= args.rpm <= 40 or not 500 <= args.ms <= 4000):
        parser.error("A step requires 10–40 RPM and 500–4000 ms.")
    if args.action == "pid" and not valid_gains(args.kp, args.ki, args.kd):
        parser.error(f"Gains must be finite and within {GAIN_RANGES}.")

    port = serial.Serial(baudrate=115200, timeout=0.035, write_timeout=0.15)
    port.port = args.port
    port.dtr = False
    port.rts = False
    try:
        port.open()
        bench = DriveBench(port)
        if args.action == "status":
            ready, fault, active, logging = bench.status()
            print(f"Ready={ready}, fault={fault}, active={active}, logging={logging}")
        elif args.action == "pid":
            side = args.wheel[0]
            print(bench.tune(f"pid {side} {args.kp:g} {args.ki:g} {args.kd:g}", f"{side}: pulses="))
            if args.save:
                print(bench.tune("save", "Tuning saved."))
        elif args.action == "save":
            print(bench.tune("save", "Tuning saved."))
        else:
            with args.csv.open("x", newline="") as output:
                count = bench.step(args.wheel[0], args.rpm, args.ms, csv.writer(output))
            print(f"Completed the {args.wheel} wheel step. {count} samples saved to {args.csv}.")
    except KeyboardInterrupt:
        return 130
    except (OSError, RuntimeError, ValueError, serial.SerialException) as error:
        print(error, file=sys.stderr)
        return 1
    finally:
        port.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
