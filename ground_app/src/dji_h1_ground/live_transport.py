"""MQTT transport adapter for live telemetry.

This module deliberately knows nothing about DTF2 fragments, mission records,
or Qt.  Paho's network callback copies each publication and hands it to a
non-blocking ingress function; all application processing happens elsewhere.
"""

from __future__ import annotations

from threading import RLock
from typing import Any, Callable, Protocol


class MqttTransportConfig(Protocol):
    """Configuration fields consumed by :class:`LiveMqttTransport`."""

    host: str
    port: int
    username: str
    password: str
    client_id: str
    uplink_topic: str
    subscribe_qos: int
    keepalive_seconds: int


PayloadHandler = Callable[[str, bytes, int], None]
StateHandler = Callable[[str, bool, str], None]


def _reason_failed(reason: Any) -> bool:
    failure = getattr(reason, "is_failure", None)
    return bool(failure) if failure is not None else reason != 0


class LiveMqttTransport:
    """Own the Paho client without performing application-level work."""

    def __init__(self, config: MqttTransportConfig,
                 payload_handler: PayloadHandler,
                 state_handler: StateHandler):
        self.config = config
        self._payload_handler = payload_handler
        self._state_handler = state_handler
        self._lock = RLock()
        self._client = None
        self._running = False

    def start(self) -> None:
        with self._lock:
            if self._running:
                return
        try:
            import paho.mqtt.client as mqtt
        except ModuleNotFoundError as exc:
            raise RuntimeError(
                "Live mode requires paho-mqtt; reinstall the viewer package"
            ) from exc

        client = mqtt.Client(
            callback_api_version=mqtt.CallbackAPIVersion.VERSION2,
            client_id=self.config.client_id, protocol=mqtt.MQTTv311)
        if self.config.username:
            client.username_pw_set(self.config.username,
                                   self.config.password or None)
        if getattr(self.config, "tls", False):
            client.tls_set(ca_certs=getattr(self.config, "tls_ca_file", None))
        client.reconnect_delay_set(min_delay=1, max_delay=30)
        client.max_queued_messages_set(getattr(self.config, "ack_queue_size", 512))
        client.on_connect = self._on_connect
        client.on_connect_fail = self._on_connect_fail
        client.on_disconnect = self._on_disconnect
        client.on_message = self._on_message
        client.on_subscribe = self._on_subscribe
        with self._lock:
            self._client = client
            self._running = True
        self._state_handler("connecting", False, "")
        try:
            client.connect_async(self.config.host, self.config.port,
                                 self.config.keepalive_seconds)
            client.loop_start()
        except Exception:
            with self._lock:
                self._client = None
                self._running = False
            self._state_handler("failed", False,
                                "MQTT transport failed to start")
            raise

    def stop(self) -> None:
        with self._lock:
            client = self._client
            self._client = None
            self._running = False
        if client is not None:
            try:
                client.disconnect()
            finally:
                client.loop_stop()
        self._state_handler("stopped", False, "")

    def publish(self, topic: str, payload: bytes, qos: int) -> None:
        """Queue one outbound publication through Paho's thread-safe API."""

        with self._lock:
            client = self._client
        if client is None:
            raise RuntimeError("MQTT client is unavailable")
        result = client.publish(topic, payload, qos=qos, retain=False)
        if result.rc != 0:
            raise RuntimeError(f"DTA1 publish failed rc={result.rc}")

    def _on_connect(self, client, _userdata, _flags, reason_code,
                    _properties) -> None:
        if _reason_failed(reason_code):
            self._state_handler(
                "connect failed", False, f"MQTT CONNACK {reason_code}")
            return
        result, _mid = client.subscribe(
            self.config.uplink_topic, qos=self.config.subscribe_qos)
        if result == 0:
            self._state_handler("subscribing", True, "")
        else:
            self._state_handler(
                "subscribe failed", True, f"MQTT subscribe rc={result}")

    def _on_connect_fail(self, _client, _userdata) -> None:
        self._state_handler(
            "reconnecting", False, "MQTT connection attempt failed")

    def _on_subscribe(self, _client, _userdata, _mid, reason_codes,
                      _properties) -> None:
        rejected = any(_reason_failed(code) for code in reason_codes)
        self._state_handler(
            "subscribe failed" if rejected else "subscribed", True,
            "broker rejected MQTT subscription" if rejected else "")

    def _on_disconnect(self, _client, _userdata, _disconnect_flags,
                       reason_code, _properties) -> None:
        with self._lock:
            running = self._running
        error = (f"MQTT disconnect {reason_code}"
                 if running and _reason_failed(reason_code) else "")
        self._state_handler("reconnecting" if running else "stopped",
                            False, error)

    def _on_message(self, _client, _userdata, mqtt_message) -> None:
        # Copy before returning: Paho owns the original message object.  The
        # handler must remain non-blocking so QoS-1 PUBACK is never coupled to
        # record decoding, disk access, snapshots, or map rendering.
        self._payload_handler(
            str(mqtt_message.topic), bytes(mqtt_message.payload),
            int(mqtt_message.qos))
