"""Provision the DTU's Socket A MQTT settings from a validated JSON file."""

from __future__ import annotations

import argparse
import json
import time
from pathlib import Path
from typing import Any

DEFAULT_CONFIG = Path(__file__).with_name("dtu_mqtt_config.local.json")


class ConfigError(ValueError):
    """Raised before serial access when the provisioning file is invalid."""


def require_object(value: Any, path: str) -> dict[str, Any]:
    if not isinstance(value, dict):
        raise ConfigError(f"{path} must be a JSON object")
    return value


def require_string(
    obj: dict[str, Any], key: str, path: str, *, min_length: int = 1,
    max_length: int = 255, forbid_at_delimiters: bool = False,
) -> str:
    value = obj.get(key)
    field = f"{path}.{key}"
    if not isinstance(value, str):
        raise ConfigError(f"{field} must be a string")
    if not min_length <= len(value.encode("utf-8")) <= max_length:
        raise ConfigError(
            f"{field} must contain {min_length}..{max_length} UTF-8 bytes"
        )
    if "\r" in value or "\n" in value:
        raise ConfigError(f"{field} must not contain CR or LF")
    if forbid_at_delimiters and "," in value:
        raise ConfigError(f"{field} must not contain a comma")
    return value


def require_int(
    obj: dict[str, Any], key: str, path: str, minimum: int, maximum: int,
) -> int:
    value = obj.get(key)
    field = f"{path}.{key}"
    # bool is a subclass of int in Python, but is never valid here.
    if isinstance(value, bool) or not isinstance(value, int):
        raise ConfigError(f"{field} must be an integer")
    if not minimum <= value <= maximum:
        raise ConfigError(f"{field} must be in {minimum}..{maximum}")
    return value


def require_bool(obj: dict[str, Any], key: str, path: str) -> bool:
    value = obj.get(key)
    if not isinstance(value, bool):
        raise ConfigError(f"{path}.{key} must be true or false")
    return value


def require_choice(
    obj: dict[str, Any], key: str, path: str, choices: tuple[Any, ...],
) -> Any:
    value = obj.get(key)
    if value not in choices:
        rendered = ", ".join(repr(item) for item in choices)
        raise ConfigError(f"{path}.{key} must be one of: {rendered}")
    return value


def load_config(path: Path) -> dict[str, Any]:
    try:
        with path.open("r", encoding="utf-8") as stream:
            root = json.load(stream)
    except FileNotFoundError as exc:
        raise ConfigError(
            f"configuration file not found: {path}\n"
            "Copy dtu_mqtt_config.example.json to "
            "dtu_mqtt_config.local.json and edit it."
        ) from exc
    except json.JSONDecodeError as exc:
        raise ConfigError(
            f"invalid JSON in {path} at line {exc.lineno}, column {exc.colno}: "
            f"{exc.msg}"
        ) from exc

    config = require_object(root, "root")
    if config.get("schema_version") != 1:
        raise ConfigError("root.schema_version must be 1")

    bridge = require_object(config.get("bridge"), "root.bridge")
    require_string(bridge, "port", "root.bridge", max_length=32)
    require_int(bridge, "baud_rate", "root.bridge", 1200, 921600)

    # These values document the production-board connection and configure the
    # DTU-side UART. The host tool cannot reconfigure the running ESP32 bridge;
    # its firmware must use the same baud rate before the next communication.
    uart = require_object(config.get("mcu_uart"), "root.mcu_uart")
    require_int(uart, "controller", "root.mcu_uart", 0, 2)
    tx_gpio = require_int(uart, "tx_gpio", "root.mcu_uart", 0, 48)
    rx_gpio = require_int(uart, "rx_gpio", "root.mcu_uart", 0, 48)
    if tx_gpio == rx_gpio:
        raise ConfigError("root.mcu_uart.tx_gpio and rx_gpio must differ")
    require_int(uart, "baud_rate", "root.mcu_uart", 1200, 921600)
    require_choice(uart, "data_bits", "root.mcu_uart", (8,))
    require_choice(uart, "stop_bits", "root.mcu_uart", (1,))
    require_choice(uart, "parity", "root.mcu_uart", ("NONE",))
    # The manual documents NFC, while the tested DTU firmware reports and
    # accepts the interface-specific value 485. Preserve that working mode.
    require_choice(uart, "flow_control", "root.mcu_uart", ("NFC", "485"))
    require_int(uart, "packet_gap_ms", "root.mcu_uart", 1, 300)
    require_int(uart, "packet_length", "root.mcu_uart", 64, 1024)

    mqtt = require_object(config.get("mqtt"), "root.mqtt")
    require_choice(mqtt, "socket", "root.mqtt", ("1A",))
    require_bool(mqtt, "enabled", "root.mqtt")
    host = require_string(
        mqtt, "host", "root.mqtt", max_length=128, forbid_at_delimiters=True
    )
    if "://" in host:
        raise ConfigError("root.mqtt.host must be a hostname, not a URL")
    require_int(mqtt, "port", "root.mqtt", 1, 65535)
    require_choice(mqtt, "protocol_version", "root.mqtt", (3, 4))
    require_bool(mqtt, "clean_session", "root.mqtt")
    require_int(mqtt, "keepalive_seconds", "root.mqtt", 30, 65535)
    require_choice(mqtt, "mode", "root.mqtt", ("STD",))
    require_string(
        mqtt, "client_id", "root.mqtt", max_length=128,
        forbid_at_delimiters=True,
    )
    require_string(
        mqtt, "username", "root.mqtt", max_length=128,
        forbid_at_delimiters=True,
    )
    # This DTU firmware rejects an empty MQAUTH password argument even when the
    # broker permits anonymous clients. Use a documented non-secret placeholder.
    require_string(
        mqtt, "password", "root.mqtt", max_length=128,
        forbid_at_delimiters=True,
    )

    publication = require_object(mqtt.get("publish"), "root.mqtt.publish")
    require_bool(publication, "enabled", "root.mqtt.publish")
    require_string(
        publication, "topic", "root.mqtt.publish", max_length=128,
        forbid_at_delimiters=True,
    )
    require_int(publication, "qos", "root.mqtt.publish", 0, 2)
    require_bool(publication, "retain", "root.mqtt.publish")

    subscription = require_object(mqtt.get("subscribe"), "root.mqtt.subscribe")
    require_bool(subscription, "enabled", "root.mqtt.subscribe")
    require_string(
        subscription, "topic", "root.mqtt.subscribe", max_length=128,
        forbid_at_delimiters=True,
    )
    require_int(subscription, "qos", "root.mqtt.subscribe", 0, 2)

    transport = require_object(config.get("transport"), "root.transport")
    require_bool(transport, "offline_cache", "root.transport")
    require_choice(
        transport, "uplink_conversion", "root.transport", ("RAW", "HEX")
    )
    require_choice(
        transport, "downlink_conversion", "root.transport", ("RAW", "HEX")
    )
    require_choice(transport, "registration_mode", "root.transport", ("OFF",))
    require_choice(transport, "heartbeat_mode", "root.transport", ("OFF",))
    return config


def flag(value: bool) -> int:
    return 1 if value else 0


def build_settings(config: dict[str, Any]) -> tuple[tuple[str, str], ...]:
    mqtt = config["mqtt"]
    publication = mqtt["publish"]
    subscription = mqtt["subscribe"]
    transport = config["transport"]
    uart = config["mcu_uart"]
    socket = mqtt["socket"]
    channel = socket[0]
    conversion = (
        f"{transport['uplink_conversion']},{transport['downlink_conversion']}"
    )

    # Authentication is deliberately attempted before any mutation. If a DTU
    # firmware revision rejects it, existing working settings stay untouched.
    return (
        (
            "MQTT authentication",
            f"AT+MQAUTH{channel}={mqtt['client_id']},{mqtt['username']},"
            f"{mqtt['password']}",
        ),
        ("disable Socket A", f"AT+SOCKEN{socket}=OFF"),
        (
            "Socket A endpoint",
            f"AT+SOCK{socket}=MQTT,{mqtt['host']},{mqtt['port']}",
        ),
        ("offline cache", f"AT+CACHE{channel}={'ON' if transport['offline_cache'] else 'OFF'}"),
        ("payload conversion", f"AT+DTCVT{channel}={conversion}"),
        (
            "MQTT protocol",
            f"AT+MQCONF{channel}={mqtt['protocol_version']},"
            f"{flag(mqtt['clean_session'])},{mqtt['keepalive_seconds']}",
        ),
        ("MQTT standard mode", f"AT+MQMD{channel}={mqtt['mode']}"),
        (
            "MQTT subscription",
            f"AT+MQSUB{channel}={flag(subscription['enabled'])},"
            f"{subscription['topic']},{subscription['qos']}",
        ),
        (
            "MQTT publication",
            f"AT+MQPUB{channel}={flag(publication['enabled'])},"
            f"{publication['topic']},{publication['qos']},"
            f"{flag(publication['retain'])}",
        ),
        ("registration packet", f"AT+REGMD{channel}={transport['registration_mode']}"),
        ("heartbeat packet", f"AT+HEARTMD{channel}={transport['heartbeat_mode']}"),
        ("enable Socket A", f"AT+SOCKEN{socket}={'ON' if mqtt['enabled'] else 'OFF'}"),
        (
            "DTU UART packetizer",
            f"AT+UARTTL{uart['controller']}={uart['packet_gap_ms']},"
            f"{uart['packet_length']}",
        ),
        (
            "DTU UART",
            f"AT+UART{uart['controller']}={uart['baud_rate']},"
            f"{uart['data_bits']},{uart['stop_bits']},{uart['parity']},"
            f"{uart['flow_control']}",
        ),
    )


def command_ok(response: bytes) -> bool:
    return response.strip().endswith(b"OK") and b"ERROR" not in response.upper()


def print_summary(path: Path, config: dict[str, Any]) -> None:
    mqtt = config["mqtt"]
    uart = config["mcu_uart"]
    print(f"Configuration valid: {path}")
    print(f"DTU UART: UART{uart['controller']} TX=GPIO{uart['tx_gpio']} "
          f"RX=GPIO{uart['rx_gpio']} at {uart['baud_rate']} baud")
    print(f"DTU packetizer: {uart['packet_gap_ms']} ms or "
          f"{uart['packet_length']} bytes")
    print(f"MQTT endpoint: {mqtt['host']}:{mqtt['port']}")
    print(f"MQTT client: {mqtt['client_id']} (password redacted)")
    print(f"Publish topic: {mqtt['publish']['topic']}")
    print(f"Subscribe topic: {mqtt['subscribe']['topic']}")


def provision(config: dict[str, Any]) -> int:
    try:
        import serial
        from probe_dtu import format_response, read_until_quiet, send_command
    except ImportError as exc:
        raise ConfigError(
            "pyserial is required to provision the DTU; install it with "
            "'python -m pip install pyserial'"
        ) from exc

    bridge = config["bridge"]
    entered = False
    completed = False
    with serial.Serial(bridge["port"], bridge["baud_rate"], timeout=0.05) as port:
        port.dtr = False
        port.rts = False
        time.sleep(2.0)
        port.reset_input_buffer()
        port.write(b"+++")
        port.flush()
        time.sleep(0.5)
        port.write(b"a")
        port.flush()
        entry = read_until_quiet(port)
        print(f"AT mode: {format_response(entry)}")
        entered = b"+ok" in entry.lower()
        if not entered:
            print("DTU did not enter AT mode; no settings were changed.")
            return 2

        try:
            for label, command in build_settings(config):
                response = send_command(port, command)
                # Print the label, never the command: MQTT authentication
                # contains credentials loaded from the ignored local file.
                print(f"{label}: {format_response(response)}")
                if not command_ok(response):
                    print(f"Configuration stopped at {label}.")
                    return 3
            completed = True
            response = send_command(port, "AT+REBOOT")
            print(f"reboot: {format_response(response)}")
        finally:
            if entered and not completed:
                response = send_command(port, "AT+EXIT")
                print(f"AT+EXIT: {format_response(response)}")
    return 0


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Validate and provision DTU MQTT settings from JSON"
    )
    parser.add_argument(
        "--config", type=Path, default=DEFAULT_CONFIG,
        help=f"configuration JSON (default: {DEFAULT_CONFIG})",
    )
    parser.add_argument(
        "--validate-only", action="store_true",
        help="validate and summarize the JSON without opening the serial port",
    )
    args = parser.parse_args()

    try:
        config = load_config(args.config.resolve())
    except (ConfigError, OSError) as exc:
        parser.error(str(exc))

    print_summary(args.config.resolve(), config)
    if args.validate_only:
        print("Validation only: the DTU was not accessed or rebooted.")
        return 0
    try:
        return provision(config)
    except ConfigError as exc:
        parser.error(str(exc))


if __name__ == "__main__":
    raise SystemExit(main())
