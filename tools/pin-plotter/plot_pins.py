#!/usr/bin/env python3
"""Plot the ESP32 pin monitor's live digital samples over a serial port."""

import argparse
from collections import deque
from time import monotonic

import matplotlib.pyplot as plt
import numpy as np
import serial

BAUD = 230400
MAX_LINE_BYTES = 192
REFRESH_MS = 40
NOMINAL_HIGH_VOLTS = 3.3


class SampleBuffer:
    """Decode complete serial lines and keep just the rolling plot window."""

    def __init__(self, window_seconds):
        self.window_us = round(window_seconds * 1_000_000)
        self.partial = b""
        self.period_us = None
        self.pins = ()
        self.samples = deque()
        self.last_data_at = None
        self.error = ""

    def feed(self, chunk):
        *lines, self.partial = (self.partial + chunk).split(b"\n")
        if len(self.partial) > MAX_LINE_BYTES:
            self.partial = b""
        for line in lines:
            if len(line) <= MAX_LINE_BYTES:
                self.read_line(line.rstrip(b"\r"))

    def read_line(self, line):
        fields = line.split(b",")
        if fields[0] == b"E" and len(fields) == 2:
            self.error = fields[1].decode("ascii", errors="replace")
            return

        if fields[0] == b"P" and 3 <= len(fields) <= 10:
            try:
                period_us = int(fields[1])
                pins = tuple(int(pin) for pin in fields[2:])
            except ValueError:
                return
            if period_us < 50 or len(set(pins)) != len(pins) or any(
                not 0 <= pin <= 48 for pin in pins
            ):
                return
            if (period_us, pins) != (self.period_us, self.pins):
                self.samples.clear()
                self.last_data_at = None
                self.period_us, self.pins = period_us, pins
            self.error = ""
            return

        if fields[0] != b"D" or len(fields) != 3 or self.period_us is None:
            return
        try:
            start_us = int(fields[1])
            levels = bytes.fromhex(fields[2].decode("ascii"))
        except ValueError:
            return
        if (
            not 0 <= start_us < 2**63
            or not 1 <= len(levels) <= 64
            or max(levels) >= 1 << len(self.pins)
        ):
            return

        if self.samples:
            previous_us = self.samples[-1][0]
            if start_us < previous_us - 64 * self.period_us:
                # The ESP32 restarted and its timestamp returned near zero.
                self.samples.clear()
            elif start_us <= previous_us:
                # Keep the reconstructed timestamps increasing across batches.
                start_us = previous_us + self.period_us
            elif start_us - previous_us > 2 * self.period_us:
                self.samples.append((previous_us + self.period_us, -1))

        self.samples.extend(
            (start_us + index * self.period_us, value) for index, value in enumerate(levels)
        )
        self.last_data_at = monotonic()
        cutoff = self.samples[-1][0] - self.window_us
        while len(self.samples) > 1 and self.samples[1][0] < cutoff:
            self.samples.popleft()


class LivePlot:
    def __init__(self, port, window_seconds):
        self.port = port
        self.window_seconds = window_seconds
        self.buffer = SampleBuffer(window_seconds)
        self.figure = plt.figure(figsize=(9, 4), layout="constrained")
        self.figure.canvas.manager.set_window_title("ESP32 Pin Monitor")
        self.status = self.figure.suptitle("")
        self.displayed_pins = ()
        self.lines = []
        self.disconnected = False
        self.last_drawn_at = None
        self.set_status("Waiting for the ESP32 pin monitor")
        self.timer = self.figure.canvas.new_timer(interval=REFRESH_MS)
        self.timer.add_callback(self.refresh)

    def set_status(self, message):
        if self.status.get_text() != message:
            self.status.set_text(message)
            self.figure.canvas.draw_idle()

    def configure_axes(self):
        for axis in self.figure.axes:
            axis.remove()
        self.displayed_pins = self.buffer.pins
        self.lines = []
        self.last_drawn_at = None
        axes = self.figure.subplots(len(self.displayed_pins), 1, sharex=True, squeeze=False)[:, 0]
        for index, (axis, pin) in enumerate(zip(axes, self.displayed_pins)):
            (line,) = axis.plot([], [], drawstyle="steps-post", color=f"C{index}", linewidth=1.5)
            self.lines.append(line)
            axis.set_ylabel(f"GPIO {pin}")
            axis.set_yticks([0, NOMINAL_HIGH_VOLTS], ["LOW", "HIGH"])
            axis.set_ylim(-0.5, 3.8)
            axis.set_xlim(-self.window_seconds, 0)
            axis.grid(axis="x", alpha=0.25)
        axes[-1].set_xlabel("Time relative to latest sample (s)")
        self.figure.set_size_inches(9, min(9, max(3, 1.25 + len(axes))), forward=True)

    def refresh(self):
        if self.disconnected:
            return
        try:
            self.buffer.feed(self.port.read(65536))
        except (OSError, serial.SerialException):
            self.disconnected = True
            self.set_status("Serial disconnected. Reconnect the ESP32 and reopen the plotter.")
            return

        if self.buffer.pins != self.displayed_pins:
            self.configure_axes()
            self.figure.canvas.draw_idle()

        if self.buffer.error:
            self.set_status(self.buffer.error)
        elif self.buffer.last_data_at is None:
            self.set_status("Waiting for the ESP32 pin monitor")
        elif monotonic() - self.buffer.last_data_at > 0.7:
            self.set_status("Waiting for samples")
        else:
            rate = 1_000_000 / self.buffer.period_us
            self.set_status(f"Live digital levels  ·  {rate:,.0f} samples/s  ·  0 V / 3.3 V nominal")

        if self.lines and self.buffer.samples and self.last_drawn_at != self.buffer.last_data_at:
            timestamps, levels = np.asarray(self.buffer.samples, dtype=np.int64).T
            seconds = (timestamps - timestamps[-1]) / 1_000_000
            for index, line in enumerate(self.lines):
                volts = ((levels >> index) & 1) * NOMINAL_HIGH_VOLTS
                volts[levels < 0] = np.nan
                line.set_data(seconds, volts)
            self.last_drawn_at = self.buffer.last_data_at
            self.figure.canvas.draw_idle()


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="Serial port, e.g. /dev/ttyUSB1 or COM5")
    parser.add_argument("--window", type=float, default=0.5, help="Seconds shown (default: 0.5)")
    args = parser.parse_args()
    if not 0.01 <= args.window <= 10:
        parser.error("--window must be between 0.01 and 10 seconds")

    port = serial.Serial(baudrate=BAUD, timeout=0)
    port.port = args.port
    port.dtr = False
    port.rts = False
    try:
        port.open()
    except (OSError, serial.SerialException) as error:
        parser.exit(1, f"Could not open {args.port}: {error}\n")

    try:
        plot = LivePlot(port, args.window)
        plot.timer.start()
        plt.show()
    finally:
        port.close()


if __name__ == "__main__":
    main()
