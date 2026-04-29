#!/usr/bin/env python3
"""Integration tests for peer-client liveness MQTT deltas.

Verifies that per-client connect / disconnect / rx events are published to
the MQTT broker as `peer/<peer_id>/client/<call>/<event>` topics, covering
the satellite axis (parent→satellite and satellite→parent) and the trunk-peer
non-propagation guarantee.

Topology used: default 3-reflector mesh (a, b, c) with satellite-mode node
(sat).  The MQTT broker is the shared `mosquitto` container on host port
11883.

Requires: Python 3.7+, paho-mqtt (stdlib + topology.py + test_trunk.py from
this directory).
"""

import json
import os
import socket
import struct
import sys
import threading
import time
import unittest

import paho.mqtt.client as mqtt_client

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import topology as T
from test_trunk import (  # noqa: E402
    ClientPeer, SatellitePeer,
    CLIENT_CALLSIGN, CLIENT_PASSWORD,
    CLIENT2_CALLSIGN, CLIENT2_PASSWORD,
    CLIENT3_CALLSIGN, CLIENT3_PASSWORD,
    SAT_SECRET,
    wait_until, get_status,
    recv_frame,
)

# ---------------------------------------------------------------------------
# Topology helpers
# ---------------------------------------------------------------------------

HOST = "127.0.0.1"
MQTT_HOST = HOST
MQTT_PORT = 11883  # host-mapped mosquitto port (default topology)

# Reflector "a" is the parent (first in sorted order)
PRIMARY = sorted(T.REFLECTORS)[0]                      # "a"
SAT_NAME = T.SATELLITE_NODE["name"]                    # "sat"
SAT_ID_NODE = T.SATELLITE_NODE["id"]                   # "SAT_NODE"

# On the satellite-mode reflector's broker, parent events appear under
# peer/PARENT/client/<call>/...
# (the parent sends "PARENT" as its id in MsgPeerHello reply to satellite).
SAT_PARENT_PEER_ID = "PARENT"

# On the parent's broker, satellite events appear under
# peer/<sat_id>/client/<call>/...
PARENT_SAT_PEER_ID = SAT_ID_NODE


def _client_port(name: str) -> int:
    return T.mapped_client_port(name)


def _sat_client_port() -> int:
    return T.sat_node_mapped_client_port()


def _sat_http() -> tuple:
    return (HOST, T.sat_node_mapped_http_port())


def _http(name: str) -> tuple:
    return (HOST, T.mapped_http_port(name))


# ---------------------------------------------------------------------------
# MQTT subscriber helper
# ---------------------------------------------------------------------------

class MqttCapture:
    """Subscribe to a wildcard topic and capture (topic, payload_dict) pairs.

    Usage::
        with MqttCapture("svxreflector/sat/peer/+/client/+/#") as cap:
            # ... trigger events ...
            time.sleep(2)
        msgs = cap.received   # list of (topic, dict) tuples
    """

    def __init__(self, topic: str, host: str = MQTT_HOST, port: int = MQTT_PORT):
        self.topic = topic
        self.host = host
        self.port = port
        self.received: list = []
        self._lock = threading.Lock()
        self._sub_event = threading.Event()
        self._mc = None

    def __enter__(self):
        self._mc = mqtt_client.Client()
        self._mc.on_connect = self._on_connect
        self._mc.on_message = self._on_message
        self._mc.connect(self.host, self.port, 60)
        self._mc.loop_start()
        assert self._sub_event.wait(timeout=5), (
            f"MQTT subscribe to '{self.topic}' timed out")
        time.sleep(0.2)  # let subscription settle
        return self

    def __exit__(self, *_):
        if self._mc:
            self._mc.loop_stop()
            self._mc.disconnect()
            self._mc = None

    def _on_connect(self, client, userdata, flags, rc):
        client.subscribe(self.topic)
        self._sub_event.set()

    def _on_message(self, client, userdata, msg):
        try:
            payload = json.loads(msg.payload)
        except (ValueError, json.JSONDecodeError):
            payload = {}
        with self._lock:
            self.received.append((msg.topic, payload))

    def wait_for(self, predicate, timeout: float = 2.0) -> bool:
        """Poll received list until predicate returns True or timeout."""
        deadline = time.monotonic() + timeout
        while time.monotonic() < deadline:
            with self._lock:
                if predicate(list(self.received)):
                    return True
            time.sleep(0.1)
        return False

    def snapshot(self) -> list:
        with self._lock:
            return list(self.received)


def _make_mqtt_client(topic: str, received: list, sub_event: threading.Event,
                      host: str = MQTT_HOST, port: int = MQTT_PORT):
    """Create and start a paho client subscribed to `topic`.

    Returns the client; caller must loop_stop/disconnect in a finally block.
    """
    mc = mqtt_client.Client()

    def on_connect(client, userdata, flags, rc):
        client.subscribe(topic)
        sub_event.set()

    def on_message(client, userdata, msg):
        try:
            payload = json.loads(msg.payload)
        except (ValueError, json.JSONDecodeError):
            payload = {}
        received.append((msg.topic, payload, time.monotonic()))

    mc.on_connect = on_connect
    mc.on_message = on_message
    mc.connect(host, port, 60)
    mc.loop_start()
    return mc


# ---------------------------------------------------------------------------
# UDP signal-strength helper (triggers publishRxUpdate on the reflector)
# ---------------------------------------------------------------------------

UDP_SIGNAL_STRENGTH = 104  # MsgUdpSignalStrengthValues::TYPE


def send_udp_siglev(client: ClientPeer, rx_id: int = 0x3F,
                    siglev: int = 80, flags: int = 0x07):
    """Send a MsgUdpSignalStrengthValues packet to trigger an rx update.

    Wire format (no framing, raw UDP):
      [type:u16][client_id:u16][seq:u16] [count:u16] [id:u8][siglev:u8][flags:u8]

    Flags: bit0=enabled, bit1=sql_open, bit2=active (all set by default).
    """
    if client.udp is None or not hasattr(client, "_udp_target"):
        raise RuntimeError("ClientPeer UDP not set up")

    # build body: vector of 1 Rx entry
    body = struct.pack("!H", 1)          # count
    body += struct.pack("!BBB", rx_id, siglev, flags)   # Rx: id, siglev, flags

    pkt = struct.pack("!HHH", UDP_SIGNAL_STRENGTH,
                      client.client_id, client._udp_seq) + body
    client._udp_seq = (client._udp_seq + 1) & 0xFFFF
    client.udp.sendto(pkt, client._udp_target)


# ---------------------------------------------------------------------------
# Test class
# ---------------------------------------------------------------------------

class TestMqttDeltas(unittest.TestCase):
    """Per-client liveness MQTT delta tests."""

    @classmethod
    def setUpClass(cls):
        """Wait for the primary reflector and satellite-mode node to be up."""
        D = "\033[2m"
        G = "\033[32m"
        RST = "\033[0m"

        sys.stderr.write(f"  {D}waiting for reflectors...{RST}")
        sys.stderr.flush()
        for name in sorted(T.REFLECTORS):
            wait_until(
                lambda n=name: _try_status(*_http(n)),
                timeout=90.0, interval=1.0,
                msg=f"reflector-{name} not ready",
            )
        # Also wait for the satellite-mode node
        wait_until(
            lambda: _try_status(*_sat_http()),
            timeout=60.0, interval=1.0,
            msg="satellite-mode node not ready",
        )
        sys.stderr.write(f"\r\033[K  {G}✔{RST} Reflectors + satellite up\n")
        sys.stderr.flush()
        sys.stderr.write(f"\033[2m{'─' * 50}\033[0m\n")

    # ------------------------------------------------------------------
    # Test 36: Parent client connect → visible on satellite broker
    # ------------------------------------------------------------------
    def test_36_parent_client_connect_visible_on_satellite(self):
        """A V2 client connecting to the parent ('a') causes the satellite
        ('sat') broker to emit peer/<PARENT>/client/<call>/connected within 2s.

        The parent sends MsgPeerClientConnected over the satellite link;
        SatelliteClient on 'sat' republishes it to its own MQTT broker under
        peer/PARENT/client/<call>/connected.
        """
        topic = f"svxreflector/{SAT_NAME}/peer/+/client/+/connected"
        received = []
        sub_event = threading.Event()

        mc = _make_mqtt_client(topic, received, sub_event)
        client = None
        try:
            self.assertTrue(sub_event.wait(timeout=5),
                            "MQTT subscribe timed out")
            time.sleep(0.3)

            client = ClientPeer()
            port = _client_port(PRIMARY)
            client.connect(HOST, port)
            client.authenticate(callsign=CLIENT_CALLSIGN,
                                password=CLIENT_PASSWORD)

            def has_event():
                return any(
                    CLIENT_CALLSIGN in t
                    for t, _, _ in received
                )

            got = _wait_for(has_event, timeout=2.0)
            self.assertTrue(
                got,
                f"satellite broker did not receive connected event for "
                f"{CLIENT_CALLSIGN} after connecting to parent; "
                f"topics received: {[t for t, _, _ in received]}",
            )

            # Verify peer_id == PARENT and payload has tg + ip
            matches = [(t, p) for t, p, _ in received
                       if CLIENT_CALLSIGN in t]
            self.assertGreater(len(matches), 0)
            topic_str, payload = matches[0]
            self.assertIn(f"peer/{SAT_PARENT_PEER_ID}/client/{CLIENT_CALLSIGN}",
                          topic_str,
                          f"Expected peer_id='{SAT_PARENT_PEER_ID}' in topic, "
                          f"got: {topic_str}")
            self.assertIn("tg", payload,
                          f"connected payload missing 'tg': {payload}")
            self.assertIn("ip", payload,
                          f"connected payload missing 'ip': {payload}")
        finally:
            if client:
                client.close()
            mc.loop_stop()
            mc.disconnect()

    # ------------------------------------------------------------------
    # Test 37: Satellite client connect → visible on parent broker
    # ------------------------------------------------------------------
    def test_37_satellite_client_connect_visible_on_parent(self):
        """A V2 client connecting to the satellite ('sat') causes the parent
        ('a') broker to emit peer/<SAT_NODE>/client/<call>/connected within 2s.

        The satellite sends MsgPeerClientConnected to the parent over the
        satellite link; SatelliteLink on the parent publishes it to its MQTT
        broker under peer/<SAT_NODE>/client/<call>/connected.
        """
        topic = f"svxreflector/{PRIMARY}/peer/+/client/+/connected"
        received = []
        sub_event = threading.Event()

        mc = _make_mqtt_client(topic, received, sub_event)
        client = None
        try:
            self.assertTrue(sub_event.wait(timeout=5),
                            "MQTT subscribe timed out")
            time.sleep(0.3)

            client = ClientPeer()
            client.connect(HOST, _sat_client_port())
            client.authenticate(callsign=CLIENT2_CALLSIGN,
                                password=CLIENT2_PASSWORD)

            def has_event():
                return any(CLIENT2_CALLSIGN in t for t, _, _ in received)

            got = _wait_for(has_event, timeout=2.0)
            self.assertTrue(
                got,
                f"parent broker did not receive connected event for "
                f"{CLIENT2_CALLSIGN} after connecting to satellite; "
                f"topics received: {[t for t, _, _ in received]}",
            )

            matches = [(t, p) for t, p, _ in received
                       if CLIENT2_CALLSIGN in t]
            topic_str, payload = matches[0]
            self.assertIn(
                f"peer/{PARENT_SAT_PEER_ID}/client/{CLIENT2_CALLSIGN}",
                topic_str,
                f"Expected peer_id='{PARENT_SAT_PEER_ID}' in topic, "
                f"got: {topic_str}",
            )
            self.assertIn("tg", payload)
            self.assertIn("ip", payload)
        finally:
            if client:
                client.close()
            mc.loop_stop()
            mc.disconnect()

    # ------------------------------------------------------------------
    # Test 38: Retained rx — late subscriber bootstraps from retained msg
    # ------------------------------------------------------------------
    def test_38_rx_retained_late_subscriber_bootstraps(self):
        """After a V2 client on the parent triggers an rx update, a NEW MQTT
        subscriber connecting to the satellite broker receives the last retained
        rx payload immediately on subscribe (retained message semantics).

        The rx topic on the satellite broker is:
          svxreflector/sat/peer/PARENT/client/<call>/rx
        """
        client = ClientPeer()
        port = _client_port(PRIMARY)

        try:
            client.connect(HOST, port)
            client.authenticate(callsign=CLIENT3_CALLSIGN,
                                password=CLIENT3_PASSWORD)
            client.setup_udp(udp_port=port)
            client.select_tg(1221)   # TG owned by primary (prefix 122)
            time.sleep(0.3)

            # Send several signal-strength packets to trigger rx updates
            for _ in range(4):
                send_udp_siglev(client)
                time.sleep(0.05)

            # Give the satellite time to receive the fanout message and
            # publish the retained rx topic
            time.sleep(1.5)

            # Now a FRESH subscriber connects — it should receive the retained
            # payload without any new rx events being fired.
            rx_topic = (
                f"svxreflector/{SAT_NAME}/peer/{SAT_PARENT_PEER_ID}"
                f"/client/{CLIENT3_CALLSIGN}/rx"
            )
            received = []
            sub2_event = threading.Event()

            mc2 = _make_mqtt_client(rx_topic, received, sub2_event)
            try:
                self.assertTrue(sub2_event.wait(timeout=5),
                                "MQTT subscribe for late-subscriber timed out")
                # The retained message must arrive within 1s of subscribing
                got = _wait_for(lambda: len(received) > 0, timeout=1.5)
                self.assertTrue(
                    got,
                    f"Late subscriber to '{rx_topic}' did not receive retained "
                    f"rx payload; the topic may not be published with retain=True "
                    f"or no rx update was generated",
                )
                _topic, payload, _ = received[0]
                self.assertIsInstance(
                    payload, dict,
                    f"retained rx payload is not a JSON object: {payload}",
                )
            finally:
                mc2.loop_stop()
                mc2.disconnect()
        finally:
            client.close()

    # ------------------------------------------------------------------
    # Test 39: Rx debounce invariant — no two messages <500ms apart
    # ------------------------------------------------------------------
    def test_39_rx_debounce_invariant(self):
        """Generating ≥10 rapid rx updates over ~1.5s on a client connected to
        the parent must produce no two consecutive messages <450ms apart on the
        satellite broker (500ms debounce, 50ms tolerance for clock skew).
        """
        topic = (
            f"svxreflector/{SAT_NAME}/peer/{SAT_PARENT_PEER_ID}"
            f"/client/{CLIENT_CALLSIGN}/rx"
        )
        received_with_ts = []
        sub_event = threading.Event()
        mc = _make_mqtt_client(topic, received_with_ts, sub_event)

        client = ClientPeer()
        port = _client_port(PRIMARY)

        try:
            self.assertTrue(sub_event.wait(timeout=5), "MQTT subscribe timed out")
            time.sleep(0.3)

            client.connect(HOST, port)
            client.authenticate(callsign=CLIENT_CALLSIGN,
                                password=CLIENT_PASSWORD)
            client.setup_udp(udp_port=port)
            client.select_tg(1220)
            time.sleep(0.2)

            # Send 12 rapid signal-strength updates over 1.5s (~125ms each)
            for _ in range(12):
                send_udp_siglev(client)
                time.sleep(0.125)

            # Wait for the debounce window to flush any buffered events
            time.sleep(0.8)

            # Collect timestamps from received messages (only rx for this call)
            timestamps = [ts for t, _, ts in received_with_ts]

            # We must have received at least 1 message (sanity check)
            self.assertGreater(
                len(timestamps), 0,
                f"No rx messages received on '{topic}' after 12 rx updates; "
                f"check satellite link is alive and rx fanout is wired",
            )

            # Check no consecutive pair is <450ms apart
            TOLERANCE_MS = 450  # 500ms debounce - 50ms tolerance
            violations = []
            for i in range(1, len(timestamps)):
                gap_ms = (timestamps[i] - timestamps[i - 1]) * 1000.0
                if gap_ms < TOLERANCE_MS:
                    violations.append((i, gap_ms))

            self.assertEqual(
                violations, [],
                f"Debounce invariant violated: consecutive rx messages were "
                f"<{TOLERANCE_MS}ms apart. Violations: "
                f"{[(i, f'{g:.0f}ms') for i, g in violations]}",
            )
        finally:
            client.close()
            mc.loop_stop()
            mc.disconnect()

    # ------------------------------------------------------------------
    # Test 41: SATELLITE_FILTER suppresses peer-client events
    # ------------------------------------------------------------------
    def test_41_satellite_filter_suppresses_peer_events(self):
        """When a fake satellite advertises SATELLITE_FILTER=1221, the parent
        must not send MsgPeerClientRx for TG 1220 (outside filter) but must
        send it for TG 222 (matches cluster TG which overlaps any prefix).

        This test operates at the wire level (fake SatellitePeer) to verify
        the server-side fanout filter logic without requiring a separate
        satellite reflector instance.
        """
        # Use a fresh fake satellite that sends filter "222" (cluster TG)
        # then a client on TG 1220 (does not match) vs TG 222 (matches).
        parent_name = PRIMARY
        sat_port = T.mapped_satellite_port(parent_name)

        SAT_FILTER_ID = "SAT_FILTER_TEST"
        FILTER_STR = "222"   # only TG 222 (cluster) passes
        TG_BLOCKED = 1220    # owned by "a" (prefix 122); NOT 222
        TG_ALLOWED = 222     # cluster TG; matches filter

        # Build helper: receive frames until timeout, collect MsgPeerClientRx
        def drain_frames(peer_sock, timeout: float = 2.0):
            """Drain frames from peer socket, return list of (type, data)."""
            msgs = []
            deadline = time.monotonic() + timeout
            peer_sock.settimeout(0.3)
            while time.monotonic() < deadline:
                try:
                    data = recv_frame(peer_sock)
                    msg_type = struct.unpack_from("!H", data, 0)[0]
                    msgs.append((msg_type, data))
                except (socket.timeout, OSError):
                    pass
            return msgs

        # MSG type IDs for peer client messages
        MSG_PEER_CLIENT_RX = 127

        sat = SatellitePeer()
        sat.connect_satellite(name=parent_name, port=sat_port)
        sat.handshake(sat_id=SAT_FILTER_ID, secret=SAT_SECRET)
        # Send filter "222" to the parent
        sat.send_filter(FILTER_STR)
        time.sleep(0.5)   # let filter install

        # First: client with BLOCKED TG
        client_blocked = ClientPeer()
        client_allowed = ClientPeer()

        try:
            port = _client_port(parent_name)

            client_blocked.connect(HOST, port)
            client_blocked.authenticate(callsign=CLIENT2_CALLSIGN,
                                        password=CLIENT2_PASSWORD)
            client_blocked.setup_udp(udp_port=port)
            client_blocked.select_tg(TG_BLOCKED)
            time.sleep(0.2)

            # Trigger several rx updates on the BLOCKED TG
            for _ in range(3):
                send_udp_siglev(client_blocked)
                time.sleep(0.05)

            # Drain up to 1.5s; no MsgPeerClientRx should arrive
            blocked_msgs = drain_frames(sat.sock, timeout=1.5)
            blocked_rx = [t for t, _ in blocked_msgs
                          if t == MSG_PEER_CLIENT_RX]
            self.assertEqual(
                blocked_rx, [],
                f"Parent sent MsgPeerClientRx (type {MSG_PEER_CLIENT_RX}) to "
                f"satellite with filter='{FILTER_STR}' for blocked TG "
                f"{TG_BLOCKED}; filter not applied at fanout",
            )

            # Now: client with ALLOWED TG (222)
            client_allowed.connect(HOST, port)
            client_allowed.authenticate(callsign=CLIENT3_CALLSIGN,
                                        password=CLIENT3_PASSWORD)
            client_allowed.setup_udp(udp_port=port)
            client_allowed.select_tg(TG_ALLOWED)
            time.sleep(0.2)

            for _ in range(3):
                send_udp_siglev(client_allowed)
                time.sleep(0.05)

            allowed_msgs = drain_frames(sat.sock, timeout=1.5)
            allowed_rx = [t for t, _ in allowed_msgs
                          if t == MSG_PEER_CLIENT_RX]
            self.assertGreater(
                len(allowed_rx), 0,
                f"Parent did NOT send MsgPeerClientRx to satellite for "
                f"allowed TG {TG_ALLOWED} with filter='{FILTER_STR}'; "
                f"either the filter is too restrictive or the fanout is not "
                f"sending rx events at all",
            )
        finally:
            client_blocked.close()
            client_allowed.close()
            sat.close()

    # ------------------------------------------------------------------
    # Test 42: Trunk peers do NOT receive peer/client/* topics
    # ------------------------------------------------------------------
    def test_42_trunk_peers_no_peerclient_topics(self):
        """Connecting a V2 client on 'a' must NOT cause any
        peer/+/client/+/* MQTT topics on reflector 'b's broker.

        Trunk peers exchange MsgPeerNodeList snapshots and talker events only;
        the new MsgPeerClient* messages are NOT forwarded to trunk peers.
        """
        # Reflector "b" is a trunk peer of "a"
        peer_b = sorted(T.REFLECTORS)[1]   # "b"

        # All reflectors share the same mosquitto broker (port 11883).
        # We subscribe to b's topic namespace to check b does not publish
        # peer/+/client/+/connected from a-side events.
        topic = f"svxreflector/{peer_b}/peer/+/client/+/#"
        received = []
        sub_event = threading.Event()

        mc = _make_mqtt_client(topic, received, sub_event)
        client = None
        try:
            self.assertTrue(sub_event.wait(timeout=5), "MQTT subscribe timed out")
            time.sleep(0.3)

            # Connect a client on the primary ("a")
            client = ClientPeer()
            client.connect(HOST, _client_port(PRIMARY))
            client.authenticate(callsign=CLIENT_CALLSIGN,
                                password=CLIENT_PASSWORD)

            # Wait 2s — no peer/client events should arrive on b's namespace
            time.sleep(2.0)

            peer_client_msgs = [t for t, _, _ in received]
            self.assertEqual(
                peer_client_msgs, [],
                f"Trunk peer '{peer_b}' published peer/+/client/* topics for "
                f"a-side client '{CLIENT_CALLSIGN}'; trunk fanout must NOT "
                f"emit MsgPeerClient* messages. Topics seen: {peer_client_msgs}",
            )
        finally:
            if client:
                client.close()
            mc.loop_stop()
            mc.disconnect()


# ---------------------------------------------------------------------------
# Shared helpers
# ---------------------------------------------------------------------------

def _try_status(host: str, port: int) -> bool:
    try:
        get_status(host, port, timeout=2.0)
        return True
    except Exception:
        return False


def _wait_for(predicate, timeout: float = 2.0, interval: float = 0.1) -> bool:
    deadline = time.monotonic() + timeout
    while time.monotonic() < deadline:
        if predicate():
            return True
        time.sleep(interval)
    return False


# ---------------------------------------------------------------------------
# Custom test runner
# ---------------------------------------------------------------------------

class MqttDeltaTestResult(unittest.TestResult):
    BOLD = "\033[1m"
    GREEN = "\033[32m"
    RED = "\033[31m"
    YELLOW = "\033[33m"
    DIM = "\033[2m"
    RESET = "\033[0m"

    def __init__(self, stream, verbosity=2):
        super().__init__(stream, False, verbosity)
        self.stream = stream
        self.start_time = None

    def startTest(self, test):
        super().startTest(test)
        self.start_time = time.monotonic()
        label = test.shortDescription() or test._testMethodName
        self.stream.write(f"  {self.DIM}running{self.RESET} {label}")
        self.stream.flush()

    def _finish(self, symbol, color, test, elapsed):
        label = test.shortDescription() or test._testMethodName
        self.stream.write(f"\r\033[K  {color}{symbol}{self.RESET} {label}")
        if elapsed >= 1.0:
            self.stream.write(f"  {self.DIM}({elapsed:.1f}s){self.RESET}")
        self.stream.write("\n")
        self.stream.flush()

    def addSuccess(self, test):
        super().addSuccess(test)
        self._finish("✔", self.GREEN, test,
                     time.monotonic() - self.start_time)

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self._finish("✘", self.RED, test,
                     time.monotonic() - self.start_time)

    def addError(self, test, err):
        super().addError(test, err)
        self._finish("!", self.RED, test, time.monotonic() - self.start_time)

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self._finish("-", self.YELLOW, test,
                     time.monotonic() - self.start_time)


class MqttDeltaTestRunner:
    def __init__(self, stream=None):
        self.stream = stream or sys.stderr

    def run(self, test):
        B = MqttDeltaTestResult.BOLD
        G = MqttDeltaTestResult.GREEN
        R = MqttDeltaTestResult.RED
        D = MqttDeltaTestResult.DIM
        RST = MqttDeltaTestResult.RESET

        result = MqttDeltaTestResult(self.stream)
        suite_start = time.monotonic()

        self.stream.write(f"\n{B}MQTT Deltas Integration Tests{RST}\n")
        self.stream.write(f"{D}{'─' * 50}{RST}\n")

        test(result)
        elapsed = time.monotonic() - suite_start

        self.stream.write(f"{D}{'─' * 50}{RST}\n")

        passed = result.testsRun - len(result.failures) - len(result.errors)
        total = result.testsRun

        if result.wasSuccessful():
            self.stream.write(
                f"{G}{B}{passed}/{total} passed{RST}  {D}({elapsed:.1f}s){RST}\n\n")
        else:
            self.stream.write(
                f"{R}{B}{passed}/{total} passed, "
                f"{len(result.failures)} failed, "
                f"{len(result.errors)} errors{RST}  "
                f"{D}({elapsed:.1f}s){RST}\n\n")

            for test_case, traceback in result.failures + result.errors:
                label = (test_case.shortDescription()
                         or test_case._testMethodName)
                self.stream.write(f"{R}{B}FAIL: {label}{RST}\n")
                lines = traceback.strip().splitlines()
                for line in lines:
                    self.stream.write(f"  {D}{line}{RST}\n")
                self.stream.write("\n")

        return result


if __name__ == "__main__":
    runner = MqttDeltaTestRunner()
    suite = unittest.TestLoader().loadTestsFromTestCase(TestMqttDeltas)
    result = runner.run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
