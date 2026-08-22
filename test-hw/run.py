#!/usr/bin/env python3
import argparse
import glob
import os
import queue
import signal
import subprocess
import sys
import termios
import threading
import time

EXPECTED_MESSAGE = "VU3CER MK68 10"


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


def serial_reader(descriptor, messages, stopping):
    pending = b""
    while not stopping.is_set():
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
            print(f"[generator] {line}", flush=True)
            messages.put(line)


def receiver_reader(process, messages):
    for raw_line in process.stdout:
        line = raw_line.rstrip("\n")
        print(f"[receiver] {line}", flush=True)
        messages.put(line)


def wait_for_message(messages, predicate, deadline):
    while time.monotonic() < deadline:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            break
        try:
            message = messages.get(timeout=min(0.25, remaining))
        except queue.Empty:
            continue
        if predicate(message):
            return message
    return None


def sleep_until_realtime(target):
    while True:
        remaining = target - time.time()
        if remaining <= 0:
            return
        time.sleep(min(remaining, 0.05 if remaining < 1 else 0.5))


def stop_receiver(process):
    if process.poll() is not None:
        return
    process.send_signal(signal.SIGTERM)
    try:
        process.wait(timeout=20)
    except subprocess.TimeoutExpired:
        process.kill()
        process.wait()


def main():
    script_directory = os.path.dirname(os.path.abspath(__file__))
    default_receiver = os.path.join(os.path.dirname(script_directory), "airspyhf-wsprd")
    parser = argparse.ArgumentParser(
        description="End-to-end ESP32-S3/Si5351 to Airspy HF+ WSPR decode test"
    )
    parser.add_argument("--port", help="ESP32 serial port; auto-detected if omitted")
    parser.add_argument("--receiver", default=default_receiver)
    parser.add_argument("--lead-seconds", type=int, default=15)
    parser.add_argument("--result-timeout", type=int, default=165)
    arguments = parser.parse_args()

    port = arguments.port or find_serial_port()
    descriptor = open_serial(port)
    serial_messages = queue.Queue()
    receiver_messages = queue.Queue()
    stopping = threading.Event()
    serial_thread = threading.Thread(
        target=serial_reader,
        args=(descriptor, serial_messages, stopping),
        daemon=True,
    )
    serial_thread.start()
    receiver = None

    try:
        time.sleep(1)
        os.write(descriptor, b"*ping*\n")
        if wait_for_message(
            serial_messages, lambda message: message == "PONG", time.monotonic() + 5
        ) is None:
            raise RuntimeError("ESP32 did not answer *ping*; check firmware and serial port")

        receiver = subprocess.Popen(
            [
                arguments.receiver,
                "-b",
                "20m",
                "-c",
                "VU3CER",
                "-g",
                "MK68xm",
                "-n",
            ],
            stdout=subprocess.PIPE,
            stderr=subprocess.STDOUT,
            text=True,
            bufsize=1,
        )
        receiver_thread = threading.Thread(
            target=receiver_reader,
            args=(receiver, receiver_messages),
            daemon=True,
        )
        receiver_thread.start()

        boundary = ((int(time.time()) + arguments.lead_seconds + 119) // 120) * 120
        boundary_text = time.strftime("%Y-%m-%d %H:%M:%S UTC", time.gmtime(boundary))
        print(f"Armed for {boundary_text}; RF remains off until *tx*", flush=True)
        sleep_until_realtime(boundary)
        os.write(descriptor, b"*tx*\n")

        if wait_for_message(
            serial_messages,
            lambda message: message.startswith("TX START "),
            time.monotonic() + 3,
        ) is None:
            raise RuntimeError("ESP32 did not acknowledge *tx*")

        decoded = wait_for_message(
            receiver_messages,
            lambda message: "Spot" in message and EXPECTED_MESSAGE in message,
            time.monotonic() + arguments.result_timeout,
        )
        if decoded is None:
            raise RuntimeError(f"receiver did not decode expected message: {EXPECTED_MESSAGE}")
        print(f"PASS: Airspy HF+ decoded hardware WSPR message {EXPECTED_MESSAGE}")
        return 0
    finally:
        if receiver is not None:
            stop_receiver(receiver)
        stopping.set()
        os.close(descriptor)


if __name__ == "__main__":
    try:
        sys.exit(main())
    except (OSError, RuntimeError) as error:
        print(f"FAIL: {error}", file=sys.stderr)
        sys.exit(1)
