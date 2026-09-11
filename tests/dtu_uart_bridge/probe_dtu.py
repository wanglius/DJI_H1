"""Read-only DTU network and MQTT configuration probe through the USB bridge."""

from __future__ import annotations

import argparse
import time

import serial


QUERIES = (
    "AT+CSQ",
    "AT+CEREG",
    "AT+CGATT",
    "AT+IP",
    "AT+CCLK",
    "AT+UART1",
    "AT+UARTTL1",
    "AT+SOCKEN1A",
    "AT+SOCK1A",
    "AT+SOCKLK=1A",
    "AT+CACHE1",
    "AT+DTCVT1",
    "AT+MQCONF1",
    "AT+MQMD1",
    "AT+MQSUB1",
    "AT+MQPUB1",
    "AT+REGMD1",
    "AT+HEARTMD1",
)


def read_until_quiet(port: serial.Serial, timeout_s: float = 2.0,
                     quiet_s: float = 0.15) -> bytes:
    deadline = time.monotonic() + timeout_s
    last_rx = time.monotonic()
    received = bytearray()
    while time.monotonic() < deadline:
        chunk = port.read(port.in_waiting or 1)
        if chunk:
            received.extend(chunk)
            last_rx = time.monotonic()
        elif received and time.monotonic() - last_rx >= quiet_s:
            break
    return bytes(received)


def send_command(port: serial.Serial, command: str) -> bytes:
    port.write(command.encode("ascii") + b"\r\n")
    port.flush()
    return read_until_quiet(port)


def format_response(data: bytes) -> str:
    text = data.decode("ascii", errors="backslashreplace").strip()
    return text if text else "<no response>"


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Query DTU network/MQTT state without changing settings")
    parser.add_argument("--port", default="COM6")
    parser.add_argument("--baud", type=int, default=115200)
    args = parser.parse_args()

    entered = False
    with serial.Serial(args.port, args.baud, timeout=0.05) as port:
        # Opening the native USB port may reset the ESP32-S3. Wait for the
        # bridge banner, then discard boot output before addressing the DTU.
        port.dtr = False
        port.rts = False
        time.sleep(2.0)
        port.reset_input_buffer()

        port.write(b"+++")
        port.flush()
        time.sleep(0.5)  # DTU manual recommends 500 ms before the final 'a'.
        port.write(b"a")
        port.flush()
        entry = read_until_quiet(port)
        print(f"AT mode: {format_response(entry)}")
        entered = b"+ok" in entry.lower()
        if not entered:
            print("DTU did not enter AT mode; no queries were sent.")
            return 2

        try:
            for command in QUERIES:
                response = send_command(port, command)
                print(f"{command}: {format_response(response)}")
        finally:
            if entered:
                response = send_command(port, "AT+EXIT")
                print(f"AT+EXIT: {format_response(response)}")

    return 0


if __name__ == "__main__":
    raise SystemExit(main())
