#!/usr/bin/env python3
"""Graph live ESP32 speed telemetry or inspect a saved bench-test CSV."""

import argparse
import csv
import math
import re
import sys
from collections import deque
from dataclasses import dataclass
from pathlib import Path
from time import monotonic

import matplotlib.pyplot as plt
from matplotlib.widgets import Button, TextBox
import serial

from bench import DriveBench, FIELDS, GAIN_RANGES, SAMPLE, valid_gains

REFRESH_MS = 75
TERM_FIELDS = tuple(f"{side}_{term}_pct" for side in ("l", "r") for term in ("p", "i", "d"))
GAINS = re.compile(
    r"([lr]): pulses=\d+ ppr=\d+ Kp=([0-9.]+) Ki=([0-9.]+) Kd=(-?[0-9.]+)"
)
DUTY_MAX = 255


@dataclass(frozen=True)
class WheelSample:
    target: float
    estimated: float
    duty: float
    terms: tuple[float, float, float]


@dataclass(frozen=True)
class Sample:
    ms: int
    left: WheelSample
    right: WheelSample

    @classmethod
    def from_fields(cls, values):
        ms, *data = values
        if len(data) not in (6, 12):
            raise ValueError("Unexpected telemetry field count.")
        numbers = tuple(map(float, data))
        if int(ms) < 0 or not all(map(math.isfinite, numbers)):
            raise ValueError("Invalid telemetry value.")
        if not all(0 <= numbers[index] <= DUTY_MAX for index in (2, 5)):
            raise ValueError("Invalid PWM value.")
        missing = (math.nan,) * 3
        wheels = (
            WheelSample(numbers[0], numbers[1], numbers[2] * 100 / DUTY_MAX, numbers[6:9] or missing),
            WheelSample(numbers[3], numbers[4], numbers[5] * 100 / DUTY_MAX, numbers[9:12] or missing),
        )
        return cls(int(ms), *wheels)

    @classmethod
    def from_serial(cls, line):
        if line.startswith("PID,"):
            return cls.from_fields(line.split(",")[1:])
        if SAMPLE.fullmatch(line):
            return cls.from_fields(line.split(","))
        return None


def read_csv(path):
    with path.open(newline="") as stream:
        rows = csv.DictReader(stream)
        if not rows.fieldnames or not set(FIELDS) <= set(rows.fieldnames):
            raise ValueError("This CSV is missing the ESP32 drive telemetry columns.")
        fields = FIELDS + TERM_FIELDS if set(TERM_FIELDS) <= set(rows.fieldnames) else FIELDS
        samples = [Sample.from_fields([row[field] for field in fields]) for row in rows]
    if not samples:
        raise ValueError("No telemetry samples in this CSV.")
    if any(new.ms < old.ms for old, new in zip(samples, samples[1:])):
        raise ValueError("The CSV must contain a single nondecreasing device timeline.")
    return samples


class SpeedPlot:
    def __init__(self, live=False):
        self.figure, self.axes = plt.subplots(
            4, 2, sharex=True, sharey="row",
            figsize=(11.5, 8.6), layout="constrained",
        )
        if live:
            self.figure.get_layout_engine().set(rect=(0, 0.16, 1, 0.84))
        self.figure.canvas.manager.set_window_title("HotelBot RPM tracking")
        self.heading = self.figure.suptitle("HotelBot wheel speed", fontweight="semibold")
        self.lines = []
        self.missing_terms = []
        for index, (wheel, color) in enumerate((("Left", "#086c66"), ("Right", "#ae5928"))):
            speed, error, terms, throttle = self.axes[:, index]
            speed.set_title(f"{wheel} wheel")
            target, = speed.plot(
                [], [], "--", linewidth=1.8, color="#454c58", label="Target", drawstyle="steps-post"
            )
            measured, = speed.plot([], [], linewidth=2, color=color, label="Estimated")
            speed.legend(loc="upper left", ncol=2, fontsize="small")
            residual, = error.plot([], [], linewidth=2, color=color)
            term_lines = tuple(
                terms.plot([], [], linewidth=1.7, color=ink, label=name)[0]
                for name, ink in (("P", "#25736a"), ("I", "#b2562e"), ("D", "#69578f"))
            )
            terms.legend(loc="upper left", ncol=3, fontsize="small")
            self.missing_terms.append(terms.text(
                .5, .45, "PID terms unavailable", transform=terms.transAxes,
                ha="center", color="#62666b", fontsize="small",
            ))
            output, = throttle.plot([], [], linewidth=2, color=color, drawstyle="steps-post")
            throttle.set_xlabel("Time (s)")
            self.lines.append((target, measured, residual, term_lines, output))
            for axis in (error, terms):
                axis.axhline(0, color="#62666b", linewidth=1, linestyle=":")
        for axis, label in zip(
            self.axes[:, 0], ("Speed (RPM)", "Error (RPM)", "PID terms (% duty)", "Throttle (%)")
        ):
            axis.set_ylabel(label)
        self.axes[3, 0].set_ylim(0, 100)
        for axis in self.axes.flat:
            axis.grid(alpha=0.22)

    def status(self, text):
        if self.heading.get_text() != text:
            self.heading.set_text(text)
            self.figure.canvas.draw_idle()

    def draw(self, samples, window_seconds=None):
        live = window_seconds is not None
        reference = samples[-1].ms if live else samples[0].ms
        seconds = [(sample.ms - reference) / 1000 for sample in samples]
        speed_peak = error_peak = 10.0
        term_peak = 2.0
        for side, lines, missing in zip(("left", "right"), self.lines, self.missing_terms):
            target_line, measured_line, error_line, term_lines, output_line = lines
            wheel = [getattr(sample, side) for sample in samples]
            target = [reading.target for reading in wheel]
            estimated = [reading.estimated for reading in wheel]
            error = [wanted - actual for wanted, actual in zip(target, estimated)]
            target_line.set_data(seconds, target)
            measured_line.set_data(seconds, estimated)
            error_line.set_data(seconds, error)
            output_line.set_data(seconds, [reading.duty for reading in wheel])
            have_terms = False
            for index, term_line in enumerate(term_lines):
                values = [reading.terms[index] for reading in wheel]
                term_line.set_data(seconds, values)
                finite = [abs(value) for value in values if math.isfinite(value)]
                if finite:
                    have_terms = True
                    term_peak = max(term_peak, max(finite))
            missing.set_visible(not have_terms)
            speed_peak = max(speed_peak, max(map(abs, target)), max(map(abs, estimated)))
            error_peak = max(error_peak, max(map(abs, error)))
        for axis, peak in zip(self.axes[:3, 0], (speed_peak, error_peak, term_peak)):
            axis.set_ylim(-1.10 * peak, 1.10 * peak)
        if live:
            self.axes[3, 0].set_xlim(-window_seconds, 0)
        else:
            self.axes[3, 0].set_xlim(0, max(0.1, seconds[-1]))
        self.figure.canvas.draw_idle()


class TuningPanel:
    def __init__(self, figure, bench):
        self.figure = figure
        self.bench = bench
        self.pending = None
        self.deadline = 0.0
        self.enabled = True
        self.note = figure.text(.035, .014, "", color="#454c58", fontsize=9)
        self.inputs = {}
        self.buttons = []
        for side, start, label in (("l", .035, "Left"), ("r", .405, "Right")):
            figure.text(start, .125, label, fontweight="semibold", fontsize=10)
            fields = []
            for offset, gain in enumerate(("Kp", "Ki", "Kd")):
                x = start + .066 * offset
                figure.text(x, .096, gain, fontsize=8)
                field = TextBox(figure.add_axes([x, .050, .060, .034]), "", initial="")
                fields.append(field)
            self.inputs[side] = fields
            self.button("Apply", start + .204, .077, lambda event, wheel=side: self.apply(wheel))
        self.button("Read gains", .764, .090, lambda event: self.request("show", {"l", "r"}))
        self.button("Save applied", .864, .106, lambda event: self.request("save", {"save"}))

    def button(self, label, x, width, clicked):
        button = Button(self.figure.add_axes([x, .050, width, .034]), label)
        button.label.set_fontsize(9)
        button.on_clicked(clicked)
        self.buttons.append(button)

    def status(self, message):
        self.note.set_text(message)
        self.figure.canvas.draw_idle()

    def request(self, command, expected):
        if not self.enabled or self.pending:
            return
        try:
            self.bench.send(command)
        except (OSError, serial.SerialException):
            self.disconnect()
            return
        self.pending = expected
        self.deadline = monotonic() + 2
        self.status("Waiting for the ESP32…")

    def apply(self, side):
        try:
            gains = [float(field.text) for field in self.inputs[side]]
            if not valid_gains(*gains):
                raise ValueError
        except ValueError:
            self.status(f"Enter gains within {GAIN_RANGES}.")
            return
        self.request(f"pid {side} " + " ".join(f"{value:.8g}" for value in gains), {side})

    def receive(self, line):
        if not self.pending:
            return
        if match := GAINS.fullmatch(line):
            side = match[1]
            if side in self.pending:
                for field, value in zip(self.inputs[side], match.groups()[1:]):
                    field.set_val(value)
                self.pending.discard(side)
                if not self.pending:
                    self.status("Controller gains received. Save applied gains to retain them after reboot.")
        elif line == "Tuning saved." and "save" in self.pending:
            self.pending = None
            self.status(line)
        elif line.startswith(("Release the joystick", "Invalid command", "Could not save")):
            self.pending = None
            self.status(line)

    def check_timeout(self):
        if self.pending and monotonic() >= self.deadline:
            self.pending = None
            self.status("No gain confirmation. Use Read gains to check the controller.")

    def disconnect(self):
        self.enabled = False
        self.pending = None
        for button in self.buttons:
            button.set_active(False)
        self.status("Reconnect and reopen the plotter to tune gains.")


def query_log_mode(bench, prior_logging):
    bench.send("log status")
    deadline = monotonic() + 0.45
    while monotonic() < deadline:
        for line in bench.lines():
            if line in ("LOG off", "LOG basic", "LOG pid"):
                return line.split()[1]
            if line.startswith("Invalid command"):
                return "legacy"
    return "basic" if prior_logging else "off"


def plot_live(port_name, window_seconds):
    port = serial.Serial(baudrate=115200, timeout=0.035, write_timeout=0.15)
    port.port = port_name
    port.dtr = False
    port.rts = False
    try:
        port.open()
        bench = DriveBench(port)
        _, _, _, prior_logging = bench.status()
        prior_mode = query_log_mode(bench, prior_logging)
        if prior_mode != "pid":
            bench.send("log on" if prior_mode == "legacy" else "log pid")
        port.timeout = 0

        graph = SpeedPlot(live=True)
        tuning = TuningPanel(graph.figure, bench)
        tuning.request("show", {"l", "r"})
        graph.status(f"Waiting for telemetry · {port_name}")
        window = deque()
        last_sample_at = None
        disconnected = False

        def refresh():
            nonlocal last_sample_at, disconnected
            if disconnected:
                return
            try:
                lines = bench.lines()
            except (OSError, serial.SerialException):
                disconnected = True
                graph.status("Serial disconnected · reopen the plotter to reconnect")
                tuning.disconnect()
                return
            changed = False
            for line in lines:
                tuning.receive(line)
                try:
                    sample = Sample.from_serial(line)
                except (ValueError, TypeError):
                    continue
                if sample is None:
                    continue
                if window and sample.ms < window[-1].ms:
                    window.clear()
                window.append(sample)
                changed = True
            tuning.check_timeout()
            if changed:
                last_sample_at = monotonic()
                while len(window) > 1 and window[1].ms < window[-1].ms - 1000 * window_seconds:
                    window.popleft()
                graph.status(f"Live wheel speed · {port_name}")
                graph.draw(window, window_seconds)
            elif last_sample_at is not None and monotonic() - last_sample_at > 1:
                graph.status(f"Waiting for telemetry · {port_name}")

        timer = graph.figure.canvas.new_timer(interval=REFRESH_MS)
        timer.add_callback(refresh)
        timer.start()
        try:
            plt.show()
        finally:
            timer.stop()
            if prior_mode != "pid" and not disconnected:
                restore = "log on" if prior_logging else "log off"
                try:
                    bench.send(restore)
                except (OSError, serial.SerialException):
                    pass
    finally:
        port.close()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    source = parser.add_mutually_exclusive_group(required=True)
    source.add_argument("--port", help="ESP32 serial port, for example /dev/ttyUSB1")
    source.add_argument("--csv", type=Path, help="CSV saved by bench.py")
    parser.add_argument("--window", type=float, default=10, help="Live history in seconds (default: 10)")
    args = parser.parse_args()
    if not 1 <= args.window <= 120:
        parser.error("--window must be between 1 and 120 seconds.")
    try:
        if args.port:
            plot_live(args.port, args.window)
        else:
            data = read_csv(args.csv)
            graph = SpeedPlot()
            graph.status(f"Recorded wheel speed · {args.csv.name}")
            graph.draw(data)
            plt.show()
    except (OSError, RuntimeError, TypeError, ValueError, serial.SerialException) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
