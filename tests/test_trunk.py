#!/usr/bin/env python3
"""Integration tests for GeuReflector trunk protocol.

Connects as a fake trunk peer via the TRUNK_TEST config section and exercises
routing, audio relay, cluster TG broadcast, and heartbeat.

Topology (prefixes, ports, secrets) is defined in topology.py — edit there
and run `python3 generate_configs.py` to regenerate configs + compose file.

Requires: Python 3.7+, stdlib only (+ topology.py from this directory).
"""

import hashlib
import hmac
import json
import os
import re
import socket
import struct
import sys
import threading
import time
import unittest
from urllib.request import urlopen
from urllib.error import URLError

# Ensure tests/ is on the path so topology can be imported
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import topology as T

# ---------------------------------------------------------------------------
# Derived endpoints from topology
# ---------------------------------------------------------------------------
HOST = "127.0.0.1"

def _trunk(name):
    return (HOST, T.mapped_trunk_port(name))

def _http(name):
    return (HOST, T.mapped_http_port(name))

REFLECTOR_NAMES = sorted(T.REFLECTORS)

TEST_SECRET = T.TEST_PEER["secret"]
TEST_PREFIX = T.prefix_str(T.TEST_PEER["prefix"])  # comma-separated for handshake
TEST_PREFIX_FIRST = T.first_prefix(T.TEST_PEER["prefix"])  # first prefix for TG generation

TEST_RX_SECRET = T.TEST_PEER_RX["secret"]
TEST_RX_PREFIX = T.prefix_str(T.TEST_PEER_RX["prefix"])

SAT_SECRET = T.SATELLITE["secret"]
SAT_ID = T.SATELLITE["id"]
ROLE_SATELLITE = 1

# V2 client auth credentials (must match [USERS]/[PASSWORDS] in configs)
CLIENT_CALLSIGN = T.TEST_CLIENTS[0]["callsign"]
CLIENT_PASSWORD = T.TEST_CLIENTS[0]["password"]
CLIENT2_CALLSIGN = T.TEST_CLIENTS[1]["callsign"]
CLIENT2_PASSWORD = T.TEST_CLIENTS[1]["password"]

# Trunk message types
MSG_TRUNK_HELLO = 115
MSG_TRUNK_TALKER_START = 116
MSG_TRUNK_TALKER_STOP = 117
MSG_TRUNK_AUDIO = 118
MSG_TRUNK_FLUSH = 119
MSG_TRUNK_HEARTBEAT = 120

# ---------------------------------------------------------------------------
# Wire format helpers
# ---------------------------------------------------------------------------

def pack_string(s: str) -> bytes:
    b = s.encode("utf-8")
    return struct.pack("!H", len(b)) + b


def unpack_string(data: bytes, offset: int):
    length = struct.unpack_from("!H", data, offset)[0]
    s = data[offset + 2 : offset + 2 + length].decode("utf-8")
    return s, offset + 2 + length


def pack_vec_u8(v: bytes) -> bytes:
    return struct.pack("!H", len(v)) + v


def unpack_vec_u8(data: bytes, offset: int):
    count = struct.unpack_from("!H", data, offset)[0]
    return data[offset + 2 : offset + 2 + count], offset + 2 + count


def send_frame(sock, payload: bytes):
    sock.sendall(struct.pack("!I", len(payload)) + payload)


def recv_exact(sock, n: int) -> bytes:
    buf = b""
    while len(buf) < n:
        chunk = sock.recv(n - len(buf))
        if not chunk:
            raise ConnectionError("connection closed")
        buf += chunk
    return buf


def recv_frame(sock) -> bytes:
    hdr = recv_exact(sock, 4)
    length = struct.unpack("!I", hdr)[0]
    return recv_exact(sock, length)


# ---------------------------------------------------------------------------
# Message builders
# ---------------------------------------------------------------------------

def build_trunk_hello(trunk_id: str, local_prefix: str, priority: int,
                      secret: str, role: int = 0) -> bytes:
    nonce = os.urandom(20)
    digest = hmac.new(secret.encode(), nonce, hashlib.sha1).digest()
    payload = struct.pack("!H", MSG_TRUNK_HELLO)
    payload += pack_string(trunk_id)
    payload += pack_string(local_prefix)
    payload += struct.pack("!I", priority)
    payload += pack_vec_u8(nonce)
    payload += pack_vec_u8(digest)
    payload += struct.pack("!B", role)
    return payload


def build_talker_start(tg: int, callsign: str) -> bytes:
    payload = struct.pack("!H", MSG_TRUNK_TALKER_START)
    payload += struct.pack("!I", tg)
    payload += pack_string(callsign)
    return payload


def build_talker_stop(tg: int) -> bytes:
    payload = struct.pack("!H", MSG_TRUNK_TALKER_STOP)
    payload += struct.pack("!I", tg)
    return payload


def build_audio(tg: int, audio: bytes) -> bytes:
    payload = struct.pack("!H", MSG_TRUNK_AUDIO)
    payload += struct.pack("!I", tg)
    payload += pack_vec_u8(audio)
    return payload


def build_flush(tg: int) -> bytes:
    payload = struct.pack("!H", MSG_TRUNK_FLUSH)
    payload += struct.pack("!I", tg)
    return payload


def build_heartbeat() -> bytes:
    return struct.pack("!H", MSG_TRUNK_HEARTBEAT)


# ---------------------------------------------------------------------------
# Message parser
# ---------------------------------------------------------------------------

def parse_msg(data: bytes):
    """Parse a frame payload into (type, fields_dict)."""
    msg_type = struct.unpack_from("!H", data, 0)[0]
    off = 2

    if msg_type == MSG_TRUNK_HELLO:
        trunk_id, off = unpack_string(data, off)
        prefix, off = unpack_string(data, off)
        priority = struct.unpack_from("!I", data, off)[0]; off += 4
        nonce, off = unpack_vec_u8(data, off)
        digest, off = unpack_vec_u8(data, off)
        role = struct.unpack_from("!B", data, off)[0]; off += 1
        return msg_type, {
            "id": trunk_id, "local_prefix": prefix, "priority": priority,
            "nonce": nonce, "digest": digest, "role": role,
        }
    elif msg_type == MSG_TRUNK_TALKER_START:
        tg = struct.unpack_from("!I", data, off)[0]; off += 4
        callsign, off = unpack_string(data, off)
        return msg_type, {"tg": tg, "callsign": callsign}
    elif msg_type == MSG_TRUNK_TALKER_STOP:
        tg = struct.unpack_from("!I", data, off)[0]; off += 4
        return msg_type, {"tg": tg}
    elif msg_type == MSG_TRUNK_AUDIO:
        tg = struct.unpack_from("!I", data, off)[0]; off += 4
        audio, off = unpack_vec_u8(data, off)
        return msg_type, {"tg": tg, "audio": audio}
    elif msg_type == MSG_TRUNK_FLUSH:
        tg = struct.unpack_from("!I", data, off)[0]; off += 4
        return msg_type, {"tg": tg}
    elif msg_type == MSG_TRUNK_HEARTBEAT:
        return msg_type, {}
    else:
        return msg_type, {"raw": data[off:]}


# ---------------------------------------------------------------------------
# TrunkPeer — fake trunk peer
# ---------------------------------------------------------------------------

class TrunkPeer:
    def __init__(self):
        self.sock = None

    def connect(self, host: str, port: int, timeout: float = 5.0):
        self.sock = socket.create_connection((host, port), timeout=timeout)
        self.sock.settimeout(timeout)

    def handshake(self, trunk_id: str = "TRUNK_TEST", local_prefix: str = TEST_PREFIX,
                  priority: int = None, secret: str = TEST_SECRET):
        if priority is None:
            priority = struct.unpack("!I", os.urandom(4))[0]
        hello = build_trunk_hello(trunk_id, local_prefix, priority, secret)
        send_frame(self.sock, hello)
        # Wait for reflector's hello back
        data = recv_frame(self.sock)
        return parse_msg(data)

    def send_talker_start(self, tg: int, callsign: str):
        send_frame(self.sock, build_talker_start(tg, callsign))

    def send_talker_stop(self, tg: int):
        send_frame(self.sock, build_talker_stop(tg))

    def send_audio(self, tg: int, audio: bytes):
        send_frame(self.sock, build_audio(tg, audio))

    def send_flush(self, tg: int):
        send_frame(self.sock, build_flush(tg))

    def send_heartbeat(self):
        send_frame(self.sock, build_heartbeat())

    def recv_msg(self, timeout: float = 5.0):
        old = self.sock.gettimeout()
        self.sock.settimeout(timeout)
        try:
            data = recv_frame(self.sock)
            return parse_msg(data)
        finally:
            self.sock.settimeout(old)

    def close(self):
        if self.sock:
            try:
                self.sock.close()
            except OSError:
                pass
            self.sock = None


# ---------------------------------------------------------------------------
# SatellitePeer — fake satellite connecting to a parent reflector
# ---------------------------------------------------------------------------

class SatellitePeer(TrunkPeer):
    """Satellite peer — same wire format as TrunkPeer but role=SATELLITE."""

    def connect_satellite(self, name=None):
        """Connect to the satellite port of a reflector."""
        if name is None:
            name = sorted(T.REFLECTORS)[0]
        host = HOST
        port = T.mapped_satellite_port(name)
        self.connect(host, port)

    def handshake(self, sat_id=SAT_ID, secret=SAT_SECRET, priority=None,
                  **kwargs):
        """Send MsgTrunkHello with ROLE_SATELLITE.

        Unlike trunk peers, the parent does NOT send a hello back —
        authentication is one-way (satellite proves identity to parent).
        """
        if priority is None:
            priority = struct.unpack("!I", os.urandom(4))[0]
        hello = build_trunk_hello(sat_id, "", priority, secret,
                                  role=ROLE_SATELLITE)
        send_frame(self.sock, hello)


# ---------------------------------------------------------------------------
# ClientPeer — fake V2 SvxLink client (no SSL)
# ---------------------------------------------------------------------------

# Client TCP message types
MSG_PROTO_VER = 5
MSG_AUTH_CHALLENGE = 10
MSG_AUTH_RESPONSE = 11
MSG_AUTH_OK = 12
MSG_SERVER_INFO = 100
MSG_TALKER_START = 104
MSG_TALKER_STOP = 105
MSG_SELECT_TG = 106
MSG_TG_MONITOR = 107

# Client UDP message types
UDP_HEARTBEAT = 1
UDP_AUDIO = 101
UDP_FLUSH = 102


class ClientPeer:
    """Minimal V2 SvxLink client — TCP auth + UDP audio receive."""

    def __init__(self):
        self.tcp = None
        self.udp = None
        self.client_id = None
        self._udp_seq = 0
        self._drain_thread = None
        self._drain_stop = threading.Event()
        self._tcp_msgs = []
        self._tcp_msgs_lock = threading.Lock()

    def connect(self, host: str, tcp_port: int, timeout: float = 5.0):
        self.tcp = socket.create_connection((host, tcp_port), timeout=timeout)
        self.tcp.settimeout(timeout)
        self._host = host
        self._tcp_port = tcp_port

    def authenticate(self, callsign: str = CLIENT_CALLSIGN,
                     password: str = CLIENT_PASSWORD):
        """V2 auth: ProtoVer → AuthChallenge → AuthResponse → AuthOk + ServerInfo."""
        # Send MsgProtoVer(2, 0)
        payload = struct.pack("!H", MSG_PROTO_VER)
        payload += struct.pack("!HH", 2, 0)
        send_frame(self.tcp, payload)

        # Receive MsgAuthChallenge
        data = recv_frame(self.tcp)
        msg_type = struct.unpack_from("!H", data, 0)[0]
        assert msg_type == MSG_AUTH_CHALLENGE, f"expected AuthChallenge, got {msg_type}"
        challenge, _ = unpack_vec_u8(data, 2)

        # Send MsgAuthResponse
        digest = hmac.new(password.encode(), challenge, hashlib.sha1).digest()
        payload = struct.pack("!H", MSG_AUTH_RESPONSE)
        payload += pack_string(callsign)
        payload += pack_vec_u8(digest)
        send_frame(self.tcp, payload)

        # Receive MsgAuthOk
        data = recv_frame(self.tcp)
        msg_type = struct.unpack_from("!H", data, 0)[0]
        assert msg_type == MSG_AUTH_OK, f"expected AuthOk, got {msg_type}"

        # Receive MsgServerInfo — extract client_id
        data = recv_frame(self.tcp)
        msg_type = struct.unpack_from("!H", data, 0)[0]
        assert msg_type == MSG_SERVER_INFO, f"expected ServerInfo, got {msg_type}"
        _reserved = struct.unpack_from("!H", data, 2)[0]
        self.client_id = struct.unpack_from("!H", data, 4)[0]

        # Drain any additional server messages (MsgNodeList etc.)
        self.tcp.settimeout(0.5)
        try:
            while True:
                recv_frame(self.tcp)
        except (socket.timeout, ConnectionError, OSError):
            pass
        self.tcp.settimeout(5.0)

        # Start background thread to drain TCP messages (heartbeats,
        # talker notifications, etc.) so the TCP buffer doesn't fill up.
        self._drain_stop.clear()
        self._drain_thread = threading.Thread(
            target=self._tcp_drain_loop, daemon=True)
        self._drain_thread.start()

    def setup_udp(self, udp_port: int = None):
        """Open UDP socket and register with reflector via heartbeat."""
        if udp_port is None:
            udp_port = self._tcp_port  # same port for client protocol
        self.udp = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.udp.settimeout(5.0)
        self._udp_target = (self._host, udp_port)

        # Send UDP heartbeat to register
        self._send_udp_heartbeat()

        # Wait for heartbeat echo
        data, _ = self.udp.recvfrom(4096)
        hdr_type = struct.unpack_from("!H", data, 0)[0]
        assert hdr_type == UDP_HEARTBEAT, f"expected UDP heartbeat, got {hdr_type}"

    def select_tg(self, tg: int):
        """Send MsgSelectTG to switch to a talk group."""
        payload = struct.pack("!H", MSG_SELECT_TG)
        payload += struct.pack("!I", tg)
        send_frame(self.tcp, payload)

    def monitor_tgs(self, tgs: list):
        """Send MsgTgMonitor with a set of TGs to passively monitor."""
        payload = struct.pack("!H", MSG_TG_MONITOR)
        payload += struct.pack("!H", len(tgs))
        for tg in sorted(tgs):
            payload += struct.pack("!I", tg)
        send_frame(self.tcp, payload)

    def recv_udp(self, timeout: float = 3.0):
        """Receive one UDP datagram, return (type, payload_bytes)."""
        self.udp.settimeout(timeout)
        data, _ = self.udp.recvfrom(65536)
        # V2 header: [type(2)][client_id(2)][seq(2)]
        msg_type = struct.unpack_from("!H", data, 0)[0]
        return msg_type, data[6:]

    def recv_udp_all(self, timeout: float = 2.0):
        """Drain all pending UDP datagrams, return list of (type, payload)."""
        msgs = []
        self.udp.settimeout(timeout)
        try:
            while True:
                msg_type, payload = self.recv_udp(timeout)
                msgs.append((msg_type, payload))
        except (socket.timeout, ConnectionError, OSError):
            pass
        return msgs

    def send_udp_audio(self, audio: bytes):
        """Send a UDP audio frame (V2 format: header + audio_data vec)."""
        body = pack_vec_u8(audio)
        pkt = struct.pack("!HHH", UDP_AUDIO, self.client_id, self._udp_seq) + body
        self._udp_seq = (self._udp_seq + 1) & 0xFFFF
        self.udp.sendto(pkt, self._udp_target)

    def send_udp_flush(self):
        """Send a UDP flush (end of audio stream)."""
        pkt = struct.pack("!HHH", UDP_FLUSH, self.client_id, self._udp_seq)
        self._udp_seq = (self._udp_seq + 1) & 0xFFFF
        self.udp.sendto(pkt, self._udp_target)

    def _send_udp_heartbeat(self):
        pkt = struct.pack("!HHH", UDP_HEARTBEAT, self.client_id, self._udp_seq)
        self._udp_seq = (self._udp_seq + 1) & 0xFFFF
        self.udp.sendto(pkt, self._udp_target)

    def get_tcp_msgs(self, msg_type=None):
        """Return captured TCP messages, optionally filtered by type."""
        with self._tcp_msgs_lock:
            if msg_type is None:
                return list(self._tcp_msgs)
            return [(t, d) for t, d in self._tcp_msgs if t == msg_type]

    def _tcp_drain_loop(self):
        """Background: read and capture TCP frames to keep connection alive."""
        while not self._drain_stop.is_set():
            try:
                if self.tcp is None:
                    break
                self.tcp.settimeout(0.5)
                data = recv_frame(self.tcp)
                msg_type = struct.unpack_from("!H", data, 0)[0]
                with self._tcp_msgs_lock:
                    self._tcp_msgs.append((msg_type, data))
            except (socket.timeout, ConnectionError, OSError):
                pass

    def close(self):
        self._drain_stop.set()
        if self._drain_thread:
            self._drain_thread.join(timeout=2.0)
            self._drain_thread = None
        for s in (self.tcp, self.udp):
            if s:
                try:
                    s.close()
                except OSError:
                    pass
        self.tcp = None
        self.udp = None


# ---------------------------------------------------------------------------
# HTTP status helpers
# ---------------------------------------------------------------------------

def get_status(host: str, port: int, timeout: float = 3.0) -> dict:
    url = f"http://{host}:{port}/status"
    with urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read())


def wait_until(predicate, timeout: float = 10.0, interval: float = 0.2,
               msg: str = "condition not met"):
    """Poll predicate() until it returns True or timeout expires."""
    deadline = time.monotonic() + timeout
    last_exc = None
    while time.monotonic() < deadline:
        try:
            if predicate():
                return
        except Exception as e:
            last_exc = e
        time.sleep(interval)
    detail = f" (last error: {last_exc})" if last_exc else ""
    raise AssertionError(f"Timed out: {msg}{detail}")


def wait_for_reflector(host: str, port: int, timeout: float = 60.0):
    """Wait until the reflector's /status endpoint responds."""
    def check():
        try:
            get_status(host, port, timeout=2.0)
            return True
        except (URLError, OSError, json.JSONDecodeError):
            return False
    wait_until(check, timeout=timeout, interval=1.0,
               msg=f"reflector at {host}:{port} not ready")


def wait_for_trunk_connected(host: str, port: int, trunk_name: str,
                             timeout: float = 30.0):
    """Wait until a specific trunk link shows connected in /status."""
    def check():
        status = get_status(host, port)
        trunks = status.get("trunks", {})
        return trunks.get(trunk_name, {}).get("connected", False)
    wait_until(check, timeout=timeout,
               msg=f"trunk {trunk_name} on {host}:{port} not connected")


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

class TestTrunkIntegration(unittest.TestCase):

    # First reflector in sorted order is the "primary" target for tests
    PRIMARY = REFLECTOR_NAMES[0]

    @classmethod
    def setUpClass(cls):
        """Wait for all reflectors to be healthy and mesh to form."""
        D = "\033[2m"
        G = "\033[32m"
        RST = "\033[0m"

        sys.stderr.write(f"  {D}waiting for reflectors...{RST}")
        sys.stderr.flush()
        for name in REFLECTOR_NAMES:
            wait_for_reflector(*_http(name), timeout=90.0)
        sys.stderr.write(f"\r\033[K  {G}\u2714{RST} Reflectors up\n")

        sys.stderr.write(f"  {D}waiting for mesh...{RST}")
        sys.stderr.flush()
        for name in REFLECTOR_NAMES:
            for peer in REFLECTOR_NAMES:
                if peer != name:
                    wait_for_trunk_connected(
                        *_http(name), T.trunk_section_name(name, peer), timeout=30.0)
        labels = "\u2194".join(n.upper() for n in REFLECTOR_NAMES)
        sys.stderr.write(f"\r\033[K  {G}\u2714{RST} Mesh connected ({labels})\n")
        sys.stderr.write(f"\033[2m{'─' * 50}\033[0m\n")

    def _connect_peer(self, target=None, **kwargs):
        """Helper: create and handshake a TrunkPeer to a reflector."""
        if target is None:
            target = self.PRIMARY
        peer = TrunkPeer()
        peer.connect(*_trunk(target))
        msg_type, fields = peer.handshake(**kwargs)
        self.assertEqual(msg_type, MSG_TRUNK_HELLO)
        return peer, fields

    def _get_test_talkers(self, name=None):
        """Get active_talkers dict from the TRUNK_TEST link on a reflector."""
        if name is None:
            name = self.PRIMARY
        status = get_status(*_http(name))
        return status["trunks"].get("TRUNK_TEST", {}).get("active_talkers", {})

    # ------------------------------------------------------------------
    # Test 1: Mesh connectivity (no harness needed)
    # ------------------------------------------------------------------
    def test_01_mesh_connectivity(self):
        """All trunk links in the mesh are connected."""
        for name in REFLECTOR_NAMES:
            status = get_status(*_http(name))
            for peer in REFLECTOR_NAMES:
                if peer == name:
                    continue
                section = T.trunk_section_name(name, peer)
                self.assertTrue(
                    status["trunks"][section]["connected"],
                    f"{section} not connected on reflector-{name}")

    # ------------------------------------------------------------------
    # Test 2: Successful handshake
    # ------------------------------------------------------------------
    def test_02_handshake_success(self):
        """Connect as test peer and receive hello back with correct prefix."""
        peer, fields = self._connect_peer()
        try:
            received = set(p.strip() for p in fields["local_prefix"].split(","))
            expected = set(T.prefix_list(T.REFLECTORS[self.PRIMARY]["prefix"]))
            self.assertEqual(received, expected)
            self.assertGreater(fields["priority"], 0)
        finally:
            peer.close()

    # ------------------------------------------------------------------
    # Test 3: Bad secret rejected
    # ------------------------------------------------------------------
    def test_03_handshake_bad_secret(self):
        """Connection with wrong secret is dropped."""
        peer = TrunkPeer()
        peer.connect(*_trunk(self.PRIMARY))
        hello = build_trunk_hello("BAD_PEER", TEST_PREFIX, 12345, "wrong_secret")
        send_frame(peer.sock, hello)
        with self.assertRaises((ConnectionError, OSError, struct.error)):
            peer.sock.settimeout(5.0)
            recv_frame(peer.sock)
        peer.close()

    # ------------------------------------------------------------------
    # Test 4: Talker start/stop visible in /status
    # ------------------------------------------------------------------
    def test_04_talker_start_stop(self):
        """TalkerStart appears in /status, TalkerStop clears it."""
        peer, _ = self._connect_peer()
        try:
            tg = int(TEST_PREFIX_FIRST + "001")
            peer.send_talker_start(tg, "TEST1")

            wait_until(lambda: self._get_test_talkers().get(str(tg)) == "TEST1",
                       timeout=5.0, msg="TalkerStart not visible in /status")

            peer.send_talker_stop(tg)

            wait_until(lambda: str(tg) not in self._get_test_talkers(),
                       timeout=5.0, msg="TalkerStop not cleared in /status")
        finally:
            peer.close()

    # ------------------------------------------------------------------
    # Test 5: Audio relay (talker lifecycle)
    # ------------------------------------------------------------------
    def test_05_audio_relay(self):
        """Full talker lifecycle: start, audio, flush, stop."""
        peer, _ = self._connect_peer()
        try:
            tg = int(TEST_PREFIX_FIRST + "002")
            peer.send_talker_start(tg, "AUDIO_TEST")

            wait_until(lambda: self._get_test_talkers().get(str(tg)) == "AUDIO_TEST",
                       timeout=5.0, msg="talker not visible")

            for _ in range(5):
                peer.send_audio(tg, b"\x00" * 160)

            peer.send_flush(tg)
            peer.send_talker_stop(tg)

            wait_until(lambda: str(tg) not in self._get_test_talkers(),
                       timeout=5.0, msg="talker not cleared after stop")
        finally:
            peer.close()

    # ------------------------------------------------------------------
    # Test 6: All cluster TGs accepted regardless of prefix overlap
    # ------------------------------------------------------------------
    def test_06_cluster_tgs_accepted(self):
        """Every configured cluster TG is accepted via trunk.

        Tests cluster TGs with different prefix relationships:
        - no prefix match  (not owned by any reflector)
        - prefix overlap   (starts with a reflector's prefix)
        - test-peer prefix (starts with the harness prefix)

        Trunk talker events are NOT re-forwarded to other peers (prevents
        loops). The receiving reflector must accept and record each locally.
        """
        peer, _ = self._connect_peer()
        try:
            # Start a talker on every configured cluster TG
            for tg in T.CLUSTER_TGS:
                peer.send_talker_start(tg, f"CLUSTER_{tg}")

            # Verify all appear on the primary reflector's TRUNK_TEST link
            def all_visible():
                talkers = self._get_test_talkers()
                return all(
                    talkers.get(str(tg)) == f"CLUSTER_{tg}"
                    for tg in T.CLUSTER_TGS
                )

            wait_until(all_visible, timeout=5.0,
                       msg="not all cluster TGs visible on primary reflector")

            # Verify NOT propagated to other peers (no trunk-to-trunk forwarding)
            time.sleep(1.0)
            for name in REFLECTOR_NAMES:
                if name == self.PRIMARY:
                    continue
                primary_section = T.trunk_section_name(name, self.PRIMARY)
                status = get_status(*_http(name))
                talkers = status["trunks"].get(primary_section, {}).get("active_talkers", {})
                for tg in T.CLUSTER_TGS:
                    self.assertNotIn(str(tg), talkers,
                        f"cluster TG {tg} should NOT propagate trunk-to-trunk "
                        f"(found on reflector-{name})")

            # Stop all and verify cleared
            for tg in T.CLUSTER_TGS:
                peer.send_talker_stop(tg)

            def all_cleared():
                talkers = self._get_test_talkers()
                return all(str(tg) not in talkers for tg in T.CLUSTER_TGS)

            wait_until(all_cleared, timeout=5.0,
                       msg="not all cluster TGs cleared")
        finally:
            peer.close()

    # ------------------------------------------------------------------
    # Test 7: Heartbeat keepalive
    # ------------------------------------------------------------------
    def test_07_heartbeat(self):
        """Sending heartbeats keeps the connection alive."""
        peer, _ = self._connect_peer()
        try:
            for _ in range(3):
                peer.send_heartbeat()
                time.sleep(1)

            tg = int(TEST_PREFIX_FIRST + "003")
            peer.send_talker_start(tg, "HB_TEST")

            wait_until(lambda: self._get_test_talkers().get(str(tg)) == "HB_TEST",
                       timeout=5.0, msg="connection died despite heartbeats")

            peer.send_talker_stop(tg)
        finally:
            peer.close()

    # ------------------------------------------------------------------
    # Test 8: Disconnect cleanup
    # ------------------------------------------------------------------
    def test_08_disconnect_cleanup(self):
        """Abrupt disconnect clears the talker from /status."""
        peer, _ = self._connect_peer()
        tg = int(TEST_PREFIX_FIRST + "004")
        peer.send_talker_start(tg, "DISCONNECT_TEST")

        wait_until(lambda: self._get_test_talkers().get(str(tg)) == "DISCONNECT_TEST",
                   timeout=5.0, msg="talker not visible before disconnect")

        # Abrupt close (no TalkerStop sent)
        peer.sock.close()
        peer.sock = None

        wait_until(lambda: str(tg) not in self._get_test_talkers(),
                   timeout=10.0, msg="talker not cleared after disconnect")

    # ------------------------------------------------------------------
    # Test 9: Audio not forwarded trunk-to-trunk (routing correctness)
    # ------------------------------------------------------------------
    def test_09_no_trunk_to_trunk_audio(self):
        """Audio from one trunk peer is NOT forwarded to another.

        By design, trunk-received audio only goes to local clients and
        satellites — never to other trunk peers (loop prevention).
        A second harness (TRUNK_TEST_RX) verifies it receives no audio
        or talker frames, while /status confirms the sender's audio was
        actually processed by the reflector.
        """
        # Sender: connect as TRUNK_TEST (prefix 9)
        sender, _ = self._connect_peer()

        # Receiver: connect as TRUNK_TEST_RX (prefix 8)
        receiver = TrunkPeer()
        receiver.connect(*_trunk(self.PRIMARY))
        rx_type, rx_fields = receiver.handshake(
            trunk_id="TRUNK_TEST_RX", local_prefix=TEST_RX_PREFIX,
            secret=TEST_RX_SECRET)
        self.assertEqual(rx_type, MSG_TRUNK_HELLO)

        try:
            tg = T.CLUSTER_TGS[0]  # cluster TG accepted from any peer
            audio_payload = b"\xAA\xBB" * 80  # distinctive pattern

            # Sender: full audio lifecycle
            sender.send_talker_start(tg, "AUDIO_SRC")

            wait_until(lambda: self._get_test_talkers().get(str(tg)) == "AUDIO_SRC",
                       timeout=5.0, msg="sender talker not visible")

            for _ in range(5):
                sender.send_audio(tg, audio_payload)
            sender.send_flush(tg)
            sender.send_talker_stop(tg)

            wait_until(lambda: str(tg) not in self._get_test_talkers(),
                       timeout=5.0, msg="sender talker not cleared")

            # Receiver: drain all pending frames — should only see heartbeats,
            # never TalkerStart, Audio, or Flush from the sender's session.
            audio_leaked = False
            talker_leaked = False
            receiver.sock.settimeout(2.0)
            try:
                while True:
                    data = recv_frame(receiver.sock)
                    msg_type, _ = parse_msg(data)
                    if msg_type == MSG_TRUNK_HEARTBEAT:
                        continue
                    if msg_type in (MSG_TRUNK_TALKER_START, MSG_TRUNK_TALKER_STOP):
                        talker_leaked = True
                    if msg_type in (MSG_TRUNK_AUDIO, MSG_TRUNK_FLUSH):
                        audio_leaked = True
            except (socket.timeout, ConnectionError, OSError):
                pass  # expected — no more frames

            self.assertFalse(talker_leaked,
                "trunk peer received TalkerStart/Stop from another trunk peer "
                "(trunk-to-trunk forwarding should not happen)")
            self.assertFalse(audio_leaked,
                "trunk peer received Audio/Flush from another trunk peer "
                "(trunk-to-trunk forwarding should not happen)")

        finally:
            sender.close()
            receiver.close()

    # ------------------------------------------------------------------
    # Test 10: Audio delivered to V2 client (end-to-end)
    # ------------------------------------------------------------------
    def test_10_audio_delivered_to_client(self):
        """Trunk audio reaches a V2 client connected to the same reflector.

        A trunk harness sends audio on a cluster TG.  A V2 client (no SSL)
        authenticated on the same reflector, monitoring that TG, must receive
        the audio as UDP datagrams — proving end-to-end delivery.
        """
        # Connect V2 client to reflector-a
        client = ClientPeer()
        client.connect(*_trunk(self.PRIMARY))  # same host, but need CLIENT port
        client.close()  # wrong port — need the client port

        client = ClientPeer()
        client_port = T.mapped_client_port(self.PRIMARY)
        client.connect(HOST, client_port)
        client.authenticate()
        client.setup_udp(udp_port=client_port)

        tg = T.CLUSTER_TGS[0]
        client.select_tg(tg)
        # Small delay for TG selection to take effect
        time.sleep(0.3)

        # Connect trunk harness and send audio
        sender, _ = self._connect_peer()
        try:
            audio_payload = b"\xDE\xAD" * 80  # distinctive 160-byte pattern

            sender.send_talker_start(tg, "E2E_TEST")

            wait_until(lambda: self._get_test_talkers().get(str(tg)) == "E2E_TEST",
                       timeout=5.0, msg="talker not visible")

            for _ in range(3):
                sender.send_audio(tg, audio_payload)
            sender.send_flush(tg)
            sender.send_talker_stop(tg)

            # Client should receive UDP audio + flush
            msgs = client.recv_udp_all(timeout=3.0)
            audio_count = sum(1 for t, _ in msgs if t == UDP_AUDIO)
            flush_count = sum(1 for t, _ in msgs if t == UDP_FLUSH)

            self.assertGreater(audio_count, 0,
                "V2 client received no audio frames from trunk sender")
            self.assertGreater(flush_count, 0,
                "V2 client received no flush after trunk audio")

        finally:
            sender.close()
            client.close()

    # ------------------------------------------------------------------
    # Test 11: Cross-reflector audio delivery
    # ------------------------------------------------------------------
    def test_11_cross_reflector_audio(self):
        """Audio from a client on reflector-a reaches a client on reflector-b.

        A V2 client on reflector-a talks on a TG owned by reflector-b.
        Reflector-a forwards via trunk. A V2 client on reflector-b
        monitoring that TG must receive the audio as UDP datagrams.
        """
        # Pick a TG owned by a DIFFERENT reflector
        other = [n for n in REFLECTOR_NAMES if n != self.PRIMARY][0]
        other_prefix = T.first_prefix(T.REFLECTORS[other]["prefix"])
        tg = int(other_prefix + "001")

        # Receiver: V2 client on the OTHER reflector, monitoring the TG
        receiver = ClientPeer()
        rx_port = T.mapped_client_port(other)
        receiver.connect(HOST, rx_port)
        receiver.authenticate(callsign=CLIENT_CALLSIGN, password=CLIENT_PASSWORD)
        receiver.setup_udp(udp_port=rx_port)
        receiver.select_tg(tg)
        time.sleep(0.3)

        # Sender: trunk harness on the PRIMARY reflector, acting as a
        # local talker (prefix matches primary → sent to the other via trunk)
        # We use the trunk harness because it reliably sustains audio.
        # The trunk harness sends TalkerStart for a TG matching the other
        # reflector's prefix — the primary reflector forwards it via TRUNK.
        #
        # But wait: the trunk harness has prefix 9, and TG tg starts with
        # other_prefix. The primary's TRUNK link to 'other' will forward
        # if isSharedTG matches. Since the harness sends TalkerStart on
        # the primary reflector, and the primary's onLocalTalkerStart...
        # Actually, the harness is a TRUNK peer, not a local client.
        # Trunk TalkerStart from harness → setTrunkTalkerForTG → does NOT
        # re-forward to other trunks.
        #
        # So we must use a V2 client as sender to trigger the local→trunk path.
        sender = ClientPeer()
        tx_port = T.mapped_client_port(self.PRIMARY)
        sender.connect(HOST, tx_port)
        sender.authenticate(callsign=CLIENT2_CALLSIGN, password=CLIENT2_PASSWORD)
        sender.setup_udp(udp_port=tx_port)
        sender.select_tg(tg)
        time.sleep(0.3)

        try:
            # Send audio — even 1 frame reaching the reflector triggers
            # the local talker → trunk forwarding → remote client path
            for _ in range(5):
                sender.send_udp_audio(b"\xBE\xEF" * 80)
                time.sleep(0.02)

            # Give time for trunk propagation
            time.sleep(1.0)

            # Receiver should have gotten at least some audio or flush
            msgs = receiver.recv_udp_all(timeout=2.0)
            audio_count = sum(1 for t, _ in msgs if t == UDP_AUDIO)
            flush_count = sum(1 for t, _ in msgs if t == UDP_FLUSH)

            self.assertGreater(audio_count + flush_count, 0,
                f"V2 client on reflector-{other} received no audio/flush "
                f"for TG {tg} sent from reflector-{self.PRIMARY}")

        finally:
            sender.close()
            receiver.close()

    # ------------------------------------------------------------------
    # Test 12: Satellite handshake and /status
    # ------------------------------------------------------------------
    def test_12_satellite_handshake(self):
        """Satellite connects to parent and appears in /status."""
        sat = SatellitePeer()
        sat.connect_satellite()
        sat.handshake()
        try:
            def sat_in_status():
                status = get_status(*_http(self.PRIMARY))
                sats = status.get("satellites", {})
                info = sats.get(SAT_ID, {})
                return info.get("authenticated", False)

            wait_until(sat_in_status, timeout=5.0,
                       msg="satellite not visible in /status")
        finally:
            sat.close()

    # ------------------------------------------------------------------
    # Test 13: Satellite audio reaches parent's local clients
    # ------------------------------------------------------------------
    def test_13_satellite_audio_to_parent(self):
        """Audio sent by satellite is received by a V2 client on the parent."""
        # V2 client on parent reflector
        client = ClientPeer()
        client_port = T.mapped_client_port(self.PRIMARY)
        client.connect(HOST, client_port)
        client.authenticate()
        client.setup_udp(udp_port=client_port)

        tg = int(TEST_PREFIX_FIRST + "010")
        client.select_tg(tg)
        time.sleep(0.3)

        # Satellite connects and sends audio
        sat = SatellitePeer()
        sat.connect_satellite()
        sat.handshake()
        try:
            sat.send_talker_start(tg, "SAT_TX")
            for _ in range(3):
                sat.send_audio(tg, b"\xCA\xFE" * 80)
            sat.send_flush(tg)
            sat.send_talker_stop(tg)

            msgs = client.recv_udp_all(timeout=3.0)
            audio_count = sum(1 for t, _ in msgs if t == UDP_AUDIO)
            self.assertGreater(audio_count, 0,
                "V2 client on parent received no audio from satellite")
        finally:
            sat.close()
            client.close()

    # ------------------------------------------------------------------
    # Test 14: Satellite receives audio from parent (trunk → satellite)
    # ------------------------------------------------------------------
    def test_14_satellite_receives_from_parent(self):
        """Trunk talker audio on the parent is forwarded to the satellite.

        Trunk harness sends audio on a cluster TG via TRUNK_TEST.
        The parent forwards trunk talker events to connected satellites.
        """
        sat = SatellitePeer()
        sat.connect_satellite()
        sat.handshake()

        sender, _ = self._connect_peer()
        try:
            tg = T.CLUSTER_TGS[0]
            sender.send_talker_start(tg, "TRUNK_TO_SAT")

            wait_until(lambda: self._get_test_talkers().get(str(tg)) == "TRUNK_TO_SAT",
                       timeout=5.0, msg="trunk talker not visible")

            for _ in range(3):
                sender.send_audio(tg, b"\xBE\xEF" * 80)
            sender.send_flush(tg)
            sender.send_talker_stop(tg)

            # Satellite should have received talker + audio + flush
            sat.sock.settimeout(3.0)
            got_audio = False
            got_talker = False
            try:
                while True:
                    data = recv_frame(sat.sock)
                    msg_type, _ = parse_msg(data)
                    if msg_type == MSG_TRUNK_TALKER_START:
                        got_talker = True
                    elif msg_type == MSG_TRUNK_AUDIO:
                        got_audio = True
                    elif msg_type == MSG_TRUNK_HEARTBEAT:
                        continue
            except (socket.timeout, ConnectionError, OSError):
                pass

            self.assertTrue(got_talker,
                "satellite did not receive TalkerStart from trunk")
            self.assertTrue(got_audio,
                "satellite did not receive audio from trunk")
        finally:
            sender.close()
            sat.close()

    # ------------------------------------------------------------------
    # Test 15: Satellite audio forwarded to trunk peers
    # ------------------------------------------------------------------
    def test_15_satellite_audio_to_trunk_peer(self):
        """Audio from satellite is forwarded by parent to trunk peers.

        Satellite sends audio for a TG owned by reflector-b. Parent
        should forward to reflector-b via trunk.
        """
        sat = SatellitePeer()
        sat.connect_satellite()
        sat.handshake()

        other = [n for n in REFLECTOR_NAMES if n != self.PRIMARY][0]
        other_prefix = T.first_prefix(T.REFLECTORS[other]["prefix"])
        tg = int(other_prefix + "010")

        try:
            sat.send_talker_start(tg, "SAT_ROUTE")
            for _ in range(3):
                sat.send_audio(tg, b"\x00" * 160)
            sat.send_flush(tg)
            sat.send_talker_stop(tg)
            time.sleep(0.5)

            # Check logs on the other reflector for trunk talker
            svc = T.service_name(other)
            lines = docker_log_lines(svc, since_lines=30)
            pat = re.compile(rf"Trunk talker start on TG #{tg}(?!\d)")
            found = any(pat.search(line) for line in lines)
            self.assertTrue(found,
                f"reflector-{other} did not receive trunk talker for TG {tg} "
                f"from satellite via parent")
        finally:
            sat.close()

    # ------------------------------------------------------------------
    # Test 16: Satellite disconnect cleanup
    # ------------------------------------------------------------------
    def test_16_satellite_disconnect_cleanup(self):
        """Abrupt satellite disconnect clears it from /status."""
        sat = SatellitePeer()
        sat.connect_satellite()
        sat.handshake()

        def sat_visible():
            status = get_status(*_http(self.PRIMARY))
            return SAT_ID in status.get("satellites", {})

        wait_until(sat_visible, timeout=5.0,
                   msg="satellite not in /status before disconnect")

        # Abrupt close
        sat.sock.close()
        sat.sock = None

        def sat_gone():
            status = get_status(*_http(self.PRIMARY))
            return SAT_ID not in status.get("satellites", {})

        wait_until(sat_gone, timeout=20.0,
                   msg="satellite not cleared from /status after disconnect")

    # ------------------------------------------------------------------
    # Test 17: Trunk talker notification reaches monitoring clients
    # ------------------------------------------------------------------
    def test_17_bidirectional_trunk_conversation(self):
        """Full bidirectional conversation through trunk.

        1. Client-A on reflector-a (prefix 122) selects TG 121001 (owned by b)
        2. Client-B on reflector-b (prefix 121) monitors TG 121001
        3. Client-A talks → audio reaches client-B via trunk (existing behavior)
        4. Client-B replies on TG 121001 → audio reaches client-A via trunk
           (new: peer interest tracking enables the return path)
        Also verifies that monitoring clients receive MsgTalkerStart for
        trunk talkers (TgMonitorFilter fix).
        """
        other = [n for n in REFLECTOR_NAMES if n != self.PRIMARY][0]
        other_prefix = T.first_prefix(T.REFLECTORS[other]["prefix"])
        foreign_tg = int(other_prefix + "001")

        # Home TG for monitoring client on the OTHER reflector
        home_prefix_other = T.first_prefix(T.REFLECTORS[other]["prefix"])
        home_tg_other = int(home_prefix_other + "000")

        # --- Client-A on PRIMARY reflector: selected on the foreign TG ---
        client_a = ClientPeer()
        a_port = T.mapped_client_port(self.PRIMARY)
        client_a.connect(HOST, a_port)
        client_a.authenticate(callsign=CLIENT_CALLSIGN,
                              password=CLIENT_PASSWORD)
        client_a.setup_udp(udp_port=a_port)
        client_a.select_tg(foreign_tg)
        time.sleep(0.3)

        # --- Client-B on OTHER reflector: home TG, monitoring foreign TG ---
        client_b = ClientPeer()
        b_port = T.mapped_client_port(other)
        client_b.connect(HOST, b_port)
        client_b.authenticate(callsign=CLIENT2_CALLSIGN,
                              password=CLIENT2_PASSWORD)
        client_b.setup_udp(udp_port=b_port)
        client_b.select_tg(home_tg_other)
        client_b.monitor_tgs([foreign_tg])
        time.sleep(0.3)

        try:
            # === Phase 1: Client-A talks on foreign TG ===
            # This establishes peer interest on reflector-b's trunk to refl-a
            for _ in range(5):
                client_a.send_udp_audio(b"\xCA\xFE" * 80)
                time.sleep(0.02)
            client_a.send_udp_flush()

            # Wait for trunk propagation
            time.sleep(2.0)

            # Client-B (monitoring) should have received MsgTalkerStart
            talker_starts = client_b.get_tcp_msgs(MSG_TALKER_START)
            found_start = any(
                struct.unpack_from("!I", d, 2)[0] == foreign_tg
                for _, d in talker_starts if len(d) >= 6
            )
            self.assertTrue(found_start,
                f"Monitoring client-B did not receive MsgTalkerStart for "
                f"TG {foreign_tg} via trunk")

            # === Phase 2: Client-B switches to foreign TG and replies ===
            client_b.select_tg(foreign_tg)
            time.sleep(0.3)

            for _ in range(5):
                client_b.send_udp_audio(b"\xBE\xEF" * 80)
                time.sleep(0.02)
            client_b.send_udp_flush()

            # Wait for trunk propagation (return path via interest tracking)
            time.sleep(2.0)

            # Client-A should receive audio on the return path
            msgs_a = client_a.recv_udp_all(timeout=2.0)
            audio_count = sum(1 for t, _ in msgs_a if t == UDP_AUDIO)
            flush_count = sum(1 for t, _ in msgs_a if t == UDP_FLUSH)

            self.assertGreater(audio_count + flush_count, 0,
                f"Client-A on {self.PRIMARY} received no audio/flush for "
                f"TG {foreign_tg} reply from {other} "
                f"(peer interest tracking failed)")

        finally:
            client_a.close()
            client_b.close()


# ---------------------------------------------------------------------------
# Custom test runner with clean, readable output
# ---------------------------------------------------------------------------

class TrunkTestResult(unittest.TestResult):
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
        self.test_times = []

    def startTest(self, test):
        super().startTest(test)
        self.start_time = time.monotonic()
        # Print test name while running
        label = test.shortDescription() or test._testMethodName
        self.stream.write(f"  {self.DIM}running{self.RESET} {label}")
        self.stream.flush()

    def _finish(self, symbol, color, test, elapsed):
        label = test.shortDescription() or test._testMethodName
        self.test_times.append((label, elapsed))
        # Clear line and rewrite with result
        self.stream.write(f"\r\033[K  {color}{symbol}{self.RESET} {label}")
        if elapsed >= 1.0:
            self.stream.write(f"  {self.DIM}({elapsed:.1f}s){self.RESET}")
        self.stream.write("\n")
        self.stream.flush()

    def addSuccess(self, test):
        super().addSuccess(test)
        self._finish("\u2714", self.GREEN, test, time.monotonic() - self.start_time)

    def addFailure(self, test, err):
        super().addFailure(test, err)
        elapsed = time.monotonic() - self.start_time
        self._finish("\u2718", self.RED, test, elapsed)

    def addError(self, test, err):
        super().addError(test, err)
        elapsed = time.monotonic() - self.start_time
        self._finish("!", self.RED, test, elapsed)

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        elapsed = time.monotonic() - self.start_time
        self._finish("-", self.YELLOW, test, elapsed)


class TrunkTestRunner:
    def __init__(self, stream=None):
        self.stream = stream or __import__("sys").stderr

    def run(self, test):
        B = TrunkTestResult.BOLD
        G = TrunkTestResult.GREEN
        R = TrunkTestResult.RED
        D = TrunkTestResult.DIM
        RST = TrunkTestResult.RESET

        result = TrunkTestResult(self.stream)
        suite_start = time.monotonic()

        self.stream.write(f"\n{B}Trunk Protocol Integration Tests{RST}\n")
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
                label = test_case.shortDescription() or test_case._testMethodName
                self.stream.write(f"{R}{B}FAIL: {label}{RST}\n")
                # Show only the last relevant line of the traceback
                lines = traceback.strip().splitlines()
                for line in lines:
                    self.stream.write(f"  {D}{line}{RST}\n")
                self.stream.write("\n")

        return result


def docker_log_lines(service: str, since_lines: int = 50) -> list:
    """Get recent docker compose log lines for a service."""
    import subprocess
    result = subprocess.run(
        ["docker", "compose", "-f", "docker-compose.test.yml",
         "logs", "--tail", str(since_lines), service],
        capture_output=True, text=True, timeout=5)
    return result.stdout.strip().splitlines()


def interactive_loop():
    """Let user manually test TGs against the running mesh via a V2 client.

    Connects a V2 client, sends audio to trigger the talker, then checks
    docker logs on each reflector for routing evidence.
    """
    B = "\033[1m"
    G = "\033[32m"
    R = "\033[31m"
    D = "\033[2m"
    RST = "\033[0m"

    target = REFLECTOR_NAMES[0]

    print(f"\n{B}Interactive TG tester{RST}  {D}(mesh is still running){RST}")
    print()
    print(f"  {B}Topology:{RST}")
    for name in REFLECTOR_NAMES:
        pfx = T.prefix_str(T.REFLECTORS[name]["prefix"])
        marker = " \u25c0 client connects here" if name == target else ""
        print(f"    reflector-{name}  {B}prefix={pfx}{RST}{D}{marker}{RST}")
    cluster = ", ".join(str(tg) for tg in T.CLUSTER_TGS)
    print(f"    {D}cluster TGs:  {B}{cluster}{RST}")
    print()
    print(f"  {D}Connects as V2 client ({CLIENT_CALLSIGN}) to reflector-{target}.")
    print(f"  Any TG can be tested — routing forwards to the owning reflector.")
    print(f"  Routing is verified via reflector logs (not /status polling).")
    print(f"  Type 'q' or press Ctrl-C to exit{RST}\n")

    while True:
        try:
            raw = input(f"{B}TG number{RST} {D}(q to quit):{RST} ").strip()
        except (EOFError, KeyboardInterrupt):
            print()
            break

        if not raw or raw.lower() == "q":
            break

        try:
            tg = int(raw)
        except ValueError:
            print(f"  {R}not a valid number{RST}")
            continue

        client = ClientPeer()
        try:
            client_port = T.mapped_client_port(target)
            client.connect(HOST, client_port)
            client.authenticate()
            client.setup_udp(udp_port=client_port)
            client.select_tg(tg)
            time.sleep(0.3)

            # Send audio to trigger the talker
            for _ in range(5):
                client.send_udp_audio(b"\x00" * 160)
                time.sleep(0.02)

            # Give the reflector time to process and forward
            time.sleep(0.5)
            client.close()
            client = None
            time.sleep(0.3)

            # Check docker logs on each reflector for routing evidence
            tg_str = str(tg)
            print(f"  {D}logs for TG {B}{tg}{RST}{D}:{RST}")
            found_local = False
            found_trunk = False

            for name in REFLECTOR_NAMES:
                svc = T.service_name(name)
                lines = docker_log_lines(svc, since_lines=30)

                events = []
                tg_pattern = re.compile(rf"TG #{tg_str}(?!\d)")
                for line in lines:
                    if not tg_pattern.search(line):
                        continue
                    # Extract the event part after the container prefix
                    parts = line.split("|", 1)
                    msg = parts[-1].strip() if len(parts) > 1 else line.strip()

                    if "Talker start" in msg and "Trunk" not in msg:
                        events.append(f"{G}local talker start{RST}")
                        found_local = True
                    elif "Talker stop" in msg and "Trunk" not in msg:
                        events.append(f"{D}local talker stop{RST}")
                    elif "Trunk talker start" in msg:
                        events.append(f"{G}trunk talker start{RST}")
                        found_trunk = True
                    elif "Trunk talker stop" in msg:
                        events.append(f"{D}trunk talker stop{RST}")

                if events:
                    print(f"    reflector-{name}: {', '.join(events)}")
                else:
                    print(f"    reflector-{name}: {D}(no events){RST}")

            # Summary
            if found_local and found_trunk:
                print(f"  {G}\u2714{RST} TG {B}{tg}{RST} routed via trunk")
            elif found_local and not found_trunk:
                target_prefixes = T.prefix_list(T.REFLECTORS[target]["prefix"])
                is_local = any(tg_str.startswith(p) for p in target_prefixes)
                if is_local:
                    print(f"  {G}\u2714{RST} TG {B}{tg}{RST} is local to reflector-{target}")
                else:
                    all_prefixes = []
                    for n in REFLECTOR_NAMES:
                        all_prefixes.extend(T.prefix_list(T.REFLECTORS[n]["prefix"]))
                    has_owner = any(tg_str.startswith(p) for p in all_prefixes)
                    if has_owner:
                        print(f"  {R}\u2718{RST} TG {B}{tg}{RST} talker started locally "
                              f"but trunk forwarding not seen in logs")
                    else:
                        print(f"  {D}\u2714{RST} TG {B}{tg}{RST} talker started locally "
                              f"{D}(no remote prefix owns this TG){RST}")
            elif not found_local:
                print(f"  {R}\u2718{RST} TG {B}{tg}{RST} no talker start seen "
                      f"{D}(audio may not have reached the reflector){RST}")

        except Exception as e:
            print(f"  {R}error: {e}{RST}")
        finally:
            if client is not None:
                client.close()

        print()


if __name__ == "__main__":
    runner = TrunkTestRunner()
    suite = unittest.TestLoader().loadTestsFromTestCase(TestTrunkIntegration)
    result = runner.run(suite)

    if not result.wasSuccessful():
        raise SystemExit(1)

    try:
        interactive_loop()
    except Exception:
        pass
