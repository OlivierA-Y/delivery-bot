#!/usr/bin/env python3
"""Provision or use HotelBot's authenticated firmware update endpoint."""

import argparse
import hashlib
import hmac
import json
import os
from pathlib import Path
import secrets
import sys
import time
from urllib.error import HTTPError, URLError
from urllib.request import ProxyHandler, Request, build_opener


DEFAULT_KEY = Path.home() / ".config" / "hotelbot" / "ota.key"
HTTP = build_opener(ProxyHandler({}))


def read_key(path):
    if path.stat().st_mode & 0o077:
        raise ValueError("The OTA key file must be private (chmod 600).")
    key = path.read_bytes()
    if len(key) != 32:
        raise ValueError("The OTA key file must contain exactly 32 bytes.")
    return key


def provision(port_name, path):
    import serial

    path.parent.mkdir(mode=0o700, parents=True, exist_ok=True)
    try:
        with os.fdopen(os.open(path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600), "wb") as stream:
            stream.write(secrets.token_bytes(32))
    except FileExistsError:
        pass
    key = read_key(path)

    port = serial.Serial(baudrate=115200, timeout=.25, write_timeout=2)
    port.dtr = False
    port.rts = False
    port.port = port_name
    with port:
        port.reset_input_buffer()
        for _ in range(6):
            port.write(b"ota status\n")
            for _ in range(4):
                if port.readline().startswith(b"OTA "):
                    break
            else:
                continue
            break
        else:
            raise TimeoutError("The ESP32 did not report OTA support over USB.")

        port.write(b"ota key " + key.hex().encode("ascii") + b"\n")
        until = time.monotonic() + 4
        while time.monotonic() < until:
            reply = port.readline().decode("ascii", errors="replace").strip()
            if reply == "OTA key saved":
                print("OTA key installed. Local key file:", path)
                return
            if reply in ("OTA stop drive before provisioning", "OTA could not save key", "OTA invalid command"):
                raise ValueError(reply)
        raise TimeoutError("The ESP32 did not confirm that it saved the OTA key.")


def get_status(host):
    with HTTP.open(f"http://{host}/status", timeout=2) as response:
        return json.load(response)


def upload(host, image, path):
    firmware = image.read_bytes()
    if not firmware or firmware[0] != 0xE9:
        raise ValueError("Expected the ESP32 application firmware.bin.")
    digest = hashlib.sha256(firmware).hexdigest()
    boot = int(get_status(host)["boot"])
    auth = hmac.digest(read_key(path), f"{boot}:{digest}".encode("ascii"), "sha256").hex()
    request = Request(f"http://{host}/update", data=firmware, headers={
        "Content-Type": "application/octet-stream",
        "X-Firmware-SHA256": digest,
        "X-Update-Auth": auth,
    }, method="POST")
    with HTTP.open(request, timeout=65) as response:
        print(response.read().decode("utf-8"))

    until = time.monotonic() + 45
    while time.monotonic() < until:
        time.sleep(1)
        try:
            if get_status(host)["boot"] != boot:
                print("HotelBot restarted and is responding.")
                return
        except (URLError, TimeoutError, OSError):
            continue
    raise TimeoutError("The update was accepted. HotelBot did not respond at the same address after restarting.")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--key", type=Path, default=DEFAULT_KEY, help="Local OTA key file")
    actions = parser.add_subparsers(dest="action", required=True)
    configure = actions.add_parser("provision", help="Install the OTA key over USB")
    configure.add_argument("port", help="ESP32 USB serial port")
    send = actions.add_parser("upload", help="Send a built firmware.bin over Wi-Fi")
    send.add_argument("host", help="HotelBot's IP address")
    send.add_argument("image", type=Path, help="Path to the built firmware.bin")
    args = parser.parse_args()
    try:
        if args.action == "provision":
            provision(args.port, args.key)
        else:
            upload(args.host, args.image, args.key)
    except HTTPError as error:
        print(error.read().decode("utf-8", errors="replace") or str(error), file=sys.stderr)
        return 1
    except (OSError, ValueError, KeyError, ImportError) as error:
        print(error, file=sys.stderr)
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
