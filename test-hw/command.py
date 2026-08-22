#!/usr/bin/env python3
import argparse
import glob
import os
import sys
import termios
import time


def find_serial_port():
    stable_candidates = sorted(glob.glob("/dev/serial/by-id/*"))
    candidates = stable_candidates or sorted(
        glob.glob("/dev/cu.usbmodem*") + glob.glob("/dev/ttyACM*")
    )
    if len(candidates) != 1:
        raise RuntimeError(
            "specify --port; auto-detection found: " + ", ".join(candidates)
        )
    return candidates[0]


def open_serial(path):
    descriptor = os.open(path, os.O_RDWR | os.O_NOCTTY | os.O_NONBLOCK)
    settings = termios.tcgetattr(descriptor)
    settings[0] = 0
    settings[1] = 0
    settings[2] = termios.CS8 | termios.CREAD | termios.CLOCAL
    settings[3] = 0
    settings[4] = termios.B115200
    settings[5] = termios.B115200
    settings[6][termios.VMIN] = 0
    settings[6][termios.VTIME] = 1
    termios.tcsetattr(descriptor, termios.TCSANOW, settings)
    termios.tcflush(descriptor, termios.TCIOFLUSH)
    return descriptor


def read_lines(descriptor, seconds):
    deadline = time.monotonic() + seconds
    pending = b""
    lines = []
    while time.monotonic() < deadline:
        try:
            data = os.read(descriptor, 512)
        except BlockingIOError:
            data = b""
        if not data:
            time.sleep(0.02)
            continue
        pending += data
        while b"\n" in pending:
            raw_line, pending = pending.split(b"\n", 1)
            line = raw_line.rstrip(b"\r").decode("utf-8", errors="replace")
            print(line)
            lines.append(line)
    return lines


def sleep_until(target):
    while True:
        remaining = target - time.time()
        if remaining <= 0:
            return
        time.sleep(min(remaining, 0.05 if remaining < 1 else 0.5))


def main():
    parser = argparse.ArgumentParser(description="Control the WSPR hardware generator")
    parser.add_argument("action", choices=("ping", "arm", "tx-now"))
    parser.add_argument("--port")
    parser.add_argument("--lead-seconds", type=int, default=5)
    arguments = parser.parse_args()
    port = arguments.port or find_serial_port()
    descriptor = open_serial(port)

    try:
        time.sleep(0.5)
        if arguments.action == "ping":
            os.write(descriptor, b"*ping*\n")
            if "PONG" not in read_lines(descriptor, 2):
                raise RuntimeError("ESP32 did not answer *ping*")
            return 0
        if arguments.action == "arm":
            boundary = ((int(time.time()) + arguments.lead_seconds + 119) // 120) * 120
            boundary_text = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(boundary))
            print(f"RF is off. Waiting to send *tx* at {boundary_text}", flush=True)
            sleep_until(boundary)
        else:
            print("Sending *tx* immediately", flush=True)
        os.write(descriptor, b"*tx*\n")
        if not any(line.startswith("TX START ") for line in read_lines(descriptor, 2)):
            raise RuntimeError("ESP32 did not acknowledge *tx*")
        return 0
    finally:
        os.close(descriptor)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
