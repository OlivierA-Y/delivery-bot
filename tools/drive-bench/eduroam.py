#!/usr/bin/env python3
"""Provision the HotelBot eduroam profile over USB."""

import argparse
import getpass
import sys
import time
import warnings

import serial


def exchange(port, command, timeout=5):
    port.write(command + b"\n")
    port.flush()
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        line = port.readline()
        if line.startswith(b"EDUROAM "):
            return line.decode("utf-8", errors="replace").strip()
    raise TimeoutError("No reply from the HotelBot eduroam firmware.")


def profile_command():
    username = input("UTORid: ").strip()
    if "@" not in username and username:
        username += "@utoronto.ca"
    account, _, realm = username.partition("@")
    encoded_user = username.encode("utf-8")
    if not account or realm != "utoronto.ca" or len(encoded_user) > 95 or any(c.isspace() for c in username):
        raise ValueError("Enter a UTORid or a full @utoronto.ca eduroam identity.")
    with warnings.catch_warnings():
        warnings.simplefilter("error", getpass.GetPassWarning)
        password = getpass.getpass("eduroam password: ")
    encoded_password = password.encode("utf-8")
    if not encoded_password or len(encoded_password) > 127 or any(c in password for c in "\r\n\0"):
        raise ValueError("The password must be 1–127 bytes without a line break.")
    return b"eduroam set " + encoded_user + b"\t" + encoded_password


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", help="ESP32 USB serial port")
    parser.add_argument("action", choices=("configure", "status", "forget"))
    args = parser.parse_args()

    try:
        port = serial.Serial(baudrate=115200, timeout=0.25, write_timeout=2)
        port.dtr = False
        port.rts = False
        port.port = args.port
        with port:
            port.reset_input_buffer()
            # Opening USB can reset the board. Probe again if the first line was sent during boot.
            try:
                status = exchange(port, b"eduroam status", timeout=1)
            except TimeoutError:
                status = exchange(port, b"eduroam status")
            if args.action == "status":
                print(status)
                return 0
            command = profile_command() if args.action == "configure" else b"eduroam forget"
            result = exchange(port, command)
            print(result)
            return 0 if result.startswith(("EDUROAM profile saved", "EDUROAM profile removed")) else 1
    except (serial.SerialException, TimeoutError, ValueError, getpass.GetPassWarning, EOFError) as error:
        print(error, file=sys.stderr)
        return 1


if __name__ == "__main__":
    sys.exit(main())
