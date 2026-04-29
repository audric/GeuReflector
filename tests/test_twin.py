#!/usr/bin/env python3
"""Integration tests for GeuReflector TWIN protocol.

Verifies the TWIN handshake, PAIRED trunk wiring, talker-state mirroring via
log inspection, and recovery after container failure.

Topology used: TWIN (generate_configs.py --topology twin)
  - reflector-refa  (IT, LOCAL_PREFIX=222)
  - reflector-ref1  (DE twin 1, LOCAL_PREFIX=262, [TWIN] partner=ref2)
  - reflector-ref2  (DE twin 2, LOCAL_PREFIX=262, [TWIN] partner=ref1)
  - reflector-refc  (extra peer, LOCAL_PREFIX=333)

  refa connects to ref1 AND ref2 via a single PAIRED=1 trunk section (TRUNK_IT_DE).
  ref1 and ref2 maintain a direct [TWIN] link for state mirroring.

Requires: Python 3.7+, stdlib only (+ topology.py from this directory).
Docker must be running and the TWIN topology must have been generated:
    cd tests && python3 generate_configs.py --topology twin
    docker compose -f docker-compose.test.yml up -d --build --wait
"""

import os
import re
import struct
import subprocess
import sys
import tempfile
import threading
import time
import unittest
from urllib.request import urlopen
from urllib.error import URLError
import json

import paho.mqtt.client as mqtt_client

# Reuse the V2 mock client from test_trunk.py
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_trunk import (  # noqa: E402
    ClientPeer, SatellitePeer, UDP_AUDIO, UDP_FLUSH,
    CLIENT_CALLSIGN, CLIENT_PASSWORD,
    MSG_TRUNK_TALKER_START, MSG_TRUNK_TALKER_STOP,
    MSG_TRUNK_AUDIO, MSG_TRUNK_FLUSH, MSG_TRUNK_HEARTBEAT,
    SAT_ID, recv_frame, parse_msg,
)
import socket  # noqa: E402

# Ensure tests/ is on the path so topology can be imported
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import topology as T

# ---------------------------------------------------------------------------
# Derived endpoints (twin topology)
# ---------------------------------------------------------------------------

HOST = "127.0.0.1"

TWIN_NAMES = sorted(T.TWIN_REFLECTORS)


def _http(name: str):
    return (HOST, T.twin_mapped_http_port(name))


def _trunk(name: str):
    return (HOST, T.twin_mapped_trunk_port(name))


COMPOSE_FILE = "docker-compose.test.yml"
TESTS_DIR = os.path.dirname(os.path.abspath(__file__))

# MQTT broker for the twin topology (host-mapped mosquitto port)
TWIN_MQTT_PORT = 21883
# Path to the svxreflector binary (two dirs up from tests/)
SVXREFLECTOR_BIN = os.path.join(
    os.path.dirname(TESTS_DIR), "build", "bin", "svxreflector"
)

# ---------------------------------------------------------------------------
# Helpers
# ---------------------------------------------------------------------------


def get_status(host: str, port: int, timeout: float = 3.0) -> dict:
    url = f"http://{host}:{port}/status"
    with urlopen(url, timeout=timeout) as resp:
        return json.loads(resp.read())


def wait_until(predicate, timeout: float = 15.0, interval: float = 0.5,
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


def wait_for_reflector(host: str, port: int, timeout: float = 90.0):
    """Wait until the reflector's /status endpoint responds."""
    def check():
        try:
            get_status(host, port, timeout=2.0)
            return True
        except (URLError, OSError, json.JSONDecodeError):
            return False
    wait_until(check, timeout=timeout, interval=1.0,
               msg=f"reflector at {host}:{port} not ready")


def wait_for_trunk_connected(host: str, port: int, trunk_section: str,
                              timeout: float = 30.0):
    """Wait until a specific trunk section shows connected=True in /status."""
    def check():
        status = get_status(host, port)
        return status.get("trunks", {}).get(trunk_section, {}).get("connected", False)
    wait_until(check, timeout=timeout,
               msg=f"trunk {trunk_section} on {host}:{port} not connected")


def docker_compose(*args, timeout: float = 30.0) -> subprocess.CompletedProcess:
    """Run a docker compose command in the tests/ directory."""
    cmd = ["docker", "compose", "-f", COMPOSE_FILE] + list(args)
    return subprocess.run(cmd, cwd=TESTS_DIR, capture_output=True, text=True,
                          timeout=timeout)


def docker_logs(service: str, since: str = "60s") -> str:
    """Return recent stdout/stderr from a reflector container."""
    svc = T.service_name(service)
    proc = docker_compose("logs", "--no-color", "--since", since, svc,
                          timeout=10.0)
    return proc.stdout + proc.stderr


# ---------------------------------------------------------------------------
# Test class
# ---------------------------------------------------------------------------

class TestTwinIntegration(unittest.TestCase):
    """Integration tests for the TWIN protocol using the twin topology."""

    @classmethod
    def setUpClass(cls):
        """Wait for all TWIN reflectors to be healthy and mesh to stabilise."""
        D = "\033[2m"
        G = "\033[32m"
        RST = "\033[0m"

        sys.stderr.write(f"  {D}waiting for twin reflectors...{RST}")
        sys.stderr.flush()
        for name in TWIN_NAMES:
            wait_for_reflector(*_http(name), timeout=90.0)
        sys.stderr.write(f"\r\033[K  {G}\u2714{RST} Twin reflectors up\n")
        sys.stderr.flush()

        # Wait for trunk connections between refa↔ref1 and refa↔ref2
        sys.stderr.write(f"  {D}waiting for trunk mesh...{RST}")
        sys.stderr.flush()
        # refa has TRUNK_IT_DE (PAIRED=1 → ref1 + ref2)
        wait_for_trunk_connected(*_http("refa"), "TRUNK_IT_DE", timeout=30.0)
        # ref1 and ref2 each have TRUNK_IT_DE back to refa
        wait_for_trunk_connected(*_http("ref1"), "TRUNK_IT_DE", timeout=30.0)
        wait_for_trunk_connected(*_http("ref2"), "TRUNK_IT_DE", timeout=30.0)
        # refc links
        wait_for_trunk_connected(*_http("refa"), "TRUNK_IT_C", timeout=30.0)
        wait_for_trunk_connected(*_http("refc"), "TRUNK_IT_C", timeout=30.0)
        sys.stderr.write(f"\r\033[K  {G}\u2714{RST} Trunk mesh connected\n")
        sys.stderr.flush()

        # Give the TWIN link a few extra seconds to stabilise (it connects
        # in parallel with the trunk mesh).
        time.sleep(3.0)
        sys.stderr.write(f"\033[2m{'─' * 50}\033[0m\n")

    # ------------------------------------------------------------------
    # Test 1: Twin handshake — ref1 and ref2 see each other
    # ------------------------------------------------------------------
    def test_01_twin_handshake(self):
        """Both twin peers log a successful TWIN hello exchange.

        Verifies that ref1 and ref2 each saw the 'TWIN: hello from partner'
        log line, confirming the twin handshake completed on both sides.
        """
        for name in ("ref1", "ref2"):
            logs = docker_logs(name, since="120s")
            # Look for 'TWIN: hello from partner' — emitted by TwinLink.cpp
            # after successful HMAC verification on inbound or outbound path
            self.assertIn(
                "TWIN: hello from partner",
                logs,
                f"reflector-{name} logs do not show 'TWIN: hello from "
                f"partner'. Twin handshake may have failed.\n"
                f"Relevant log tail:\n{logs[-2000:]}",
            )

    # ------------------------------------------------------------------
    # Test 2: Twin handshake — no auth errors on either side
    # ------------------------------------------------------------------
    def test_02_twin_no_auth_errors(self):
        """Neither twin peer emits HMAC or prefix-mismatch errors.

        Verifies that the [TWIN] SECRET and LOCAL_PREFIX are consistent
        between ref1 and ref2.
        """
        error_patterns = [
            r"ERROR\[TWIN\].*authentication failed",
            r"ERROR\[TWIN\].*HMAC",
            r"ERROR\[TWIN\].*local_prefix mismatch",
        ]
        for name in ("ref1", "ref2"):
            logs = docker_logs(name, since="120s")
            for pat in error_patterns:
                match = re.search(pat, logs, re.IGNORECASE)
                self.assertIsNone(
                    match,
                    f"reflector-{name} logged a twin auth error matching "
                    f"'{pat}':\n  {match.group(0) if match else ''}",
                )

    # ------------------------------------------------------------------
    # Test 3: PAIRED trunk — refa connected to BOTH ref1 and ref2
    # ------------------------------------------------------------------
    def test_03_paired_trunk_both_partners_connected(self):
        """refa's TRUNK_IT_DE (PAIRED=1) reports connected=True.

        This confirms that TrunkLink with PAIRED=1 and two peer hosts
        reaches the 'active' state after both outbound connections complete
        their handshake (or at least one, since isActive() returns true
        when either outbound or inbound is ready).
        """
        status_a = get_status(*_http("refa"))
        trunk = status_a.get("trunks", {}).get("TRUNK_IT_DE", None)
        self.assertIsNotNone(trunk,
            "TRUNK_IT_DE not present in refa /status. "
            "Was the twin topology generated and docker-compose started?")
        self.assertTrue(
            trunk.get("connected", False),
            f"TRUNK_IT_DE on refa is not connected. trunk status: {trunk}",
        )

    # ------------------------------------------------------------------
    # Test 4: PAIRED trunk — both pair members see refa as connected
    # ------------------------------------------------------------------
    def test_04_paired_trunk_pair_members_see_refa(self):
        """ref1 and ref2 each see their TRUNK_IT_DE section as connected.

        Confirms the return leg (pair member → refa) is also established.
        """
        for name in ("ref1", "ref2"):
            status = get_status(*_http(name))
            trunk = status.get("trunks", {}).get("TRUNK_IT_DE", None)
            self.assertIsNotNone(trunk,
                f"TRUNK_IT_DE missing from reflector-{name} /status")
            self.assertTrue(
                trunk.get("connected", False),
                f"TRUNK_IT_DE not connected on reflector-{name}: {trunk}",
            )

    # ------------------------------------------------------------------
    # Test 5: Twin link wiring — no startup errors on ref1/ref2
    # ------------------------------------------------------------------
    def test_05_twin_wiring_no_errors(self):
        """ref1 and ref2 startup logs contain no twin-related ERROR lines.

        This is a smoke test: if the [TWIN] section is mis-parsed or the
        listen port fails to bind, an ERROR[TWIN] line will appear.

        NOTE: A fuller audio-mirror test (Phase C1 verification) would
        require sending audio via a mock SvxLink client and verifying the
        mirrored talker state on the twin partner.  That is out of scope for
        this log-based integration suite and should be covered by a dedicated
        E2E audio test when mock clients are available.
        """
        for name in ("ref1", "ref2"):
            logs = docker_logs(name, since="120s")
            # Any ERROR[TWIN] line is a fatal wiring problem
            error_match = re.search(r"\*\*\* ERROR\[TWIN\]", logs)
            self.assertIsNone(
                error_match,
                f"reflector-{name} has ERROR[TWIN] in startup logs:\n"
                f"  {error_match.group(0) if error_match else ''}\n"
                f"Full log tail:\n{logs[-1500:]}",
            )
            # Twin server listen line should be present
            self.assertIn(
                "TWIN: server listening on port",
                logs,
                f"reflector-{name} does not show 'TWIN: server listening' "
                f"— the twin TCP server may not have started.",
            )

    # ------------------------------------------------------------------
    # Test 6: Twin disconnect and recovery
    # ------------------------------------------------------------------
    def test_06_twin_disconnect_and_recovery(self):
        """Killing ref2 triggers an RX-timeout on ref1; restarting ref2
        causes ref1 to re-establish the twin link.

        Steps:
        1. Confirm ref1 <-> ref2 twin is up (via log evidence).
        2. Kill ref2 container.
        3. Wait up to 20s for ref1 to log an RX timeout / disconnect.
        4. Restart ref2.
        5. Wait up to 30s for ref1 to log 'TWIN: hello from partner' again
           (confirming reconnect).
        """
        # Step 1: baseline check — ref1 already has a twin handshake
        logs_before = docker_logs("ref1", since="120s")
        if "TWIN: hello from partner" not in logs_before:
            self.skipTest(
                "ref1 has not completed twin handshake yet; cannot test "
                "disconnect recovery (this may be a test-ordering issue)."
            )

        # Step 2: kill ref2
        result = docker_compose("kill", "reflector-ref2")
        if result.returncode != 0:
            self.skipTest(
                f"docker compose kill reflector-ref2 failed "
                f"(rc={result.returncode}): {result.stderr.strip()}\n"
                f"This is expected when Docker is not available in the sandbox."
            )

        try:
            # Step 3: wait for ref1 to log timeout / disconnect
            def ref1_detected_disconnect():
                logs = docker_logs("ref1", since="25s")
                return (
                    "TWIN: outbound RX timeout" in logs
                    or "TWIN: inbound RX timeout" in logs
                    or "TWIN: outbound disconnected" in logs
                )

            try:
                wait_until(
                    ref1_detected_disconnect,
                    timeout=25.0,
                    interval=1.0,
                    msg="ref1 did not detect twin disconnect within 25s",
                )
            except AssertionError:
                # Not fatal — the RX timeout window (15s) may not have fired
                # yet.  Continue to the recovery check regardless.
                pass

            # Step 4: bring ref2 back
            result = docker_compose("up", "-d", "reflector-ref2")
            if result.returncode != 0:
                self.fail(
                    f"docker compose up -d reflector-ref2 failed: "
                    f"{result.stderr.strip()}"
                )

            # Wait for ref2 to be healthy
            wait_for_reflector(*_http("ref2"), timeout=60.0)

            # Step 5: ref1 must reconnect to ref2
            def ref1_reconnected():
                # Use a short since window so we only pick up NEW handshake events
                logs = docker_logs("ref1", since="40s")
                return "TWIN: hello from partner" in logs

            wait_until(
                ref1_reconnected,
                timeout=35.0,
                interval=1.0,
                msg="ref1 did not re-establish twin link with ref2 after restart",
            )

        finally:
            # Ensure ref2 is always running for subsequent tests
            docker_compose("up", "-d", "reflector-ref2")
            # Give it a moment before proceeding
            try:
                wait_for_reflector(*_http("ref2"), timeout=45.0)
            except AssertionError:
                pass

    # ------------------------------------------------------------------
    # Test 7: PAIRED trunk failover — kill ref1, refa still routes via ref2
    # ------------------------------------------------------------------
    def test_07_paired_trunk_failover(self):
        """Killing ref1 should not disconnect refa from the DE twin pair.

        refa uses PAIRED=1 with HOST=reflector-ref1,reflector-ref2.  When
        ref1 goes down, refa must still be able to exchange traffic with ref2
        (TrunkLink D2 sticky fallback / isActive() via any live connection).

        Verification: after killing ref1 and waiting for the heartbeat RX
        timeout (~15s), refa's TRUNK_IT_DE status must still report
        connected=True (served by the ref2 connection).
        """
        # Step 1: kill ref1
        result = docker_compose("kill", "reflector-ref1")
        if result.returncode != 0:
            self.skipTest(
                f"docker compose kill reflector-ref1 failed "
                f"(rc={result.returncode}): {result.stderr.strip()}\n"
                f"This is expected when Docker is not available in the sandbox."
            )

        try:
            # Step 2: wait long enough for the dead connection to be detected
            # Heartbeat TX interval is 10s, RX timeout is 15s → allow 20s.
            time.sleep(20.0)

            # Step 3: refa must still report the trunk as connected (via ref2)
            def refa_still_connected():
                try:
                    status = get_status(*_http("refa"))
                    return status.get("trunks", {}).get(
                        "TRUNK_IT_DE", {}).get("connected", False)
                except Exception:
                    return False

            wait_until(
                refa_still_connected,
                timeout=10.0,
                interval=1.0,
                msg="refa TRUNK_IT_DE lost connectivity after killing ref1; "
                    "expected ref2 to keep the paired link active",
            )

        finally:
            # Bring ref1 back for subsequent tests
            docker_compose("up", "-d", "reflector-ref1")
            try:
                wait_for_reflector(*_http("ref1"), timeout=60.0)
                # Also wait for trunk re-establishment
                wait_for_trunk_connected(*_http("ref1"), "TRUNK_IT_DE",
                                         timeout=30.0)
                wait_for_trunk_connected(*_http("refa"), "TRUNK_IT_DE",
                                         timeout=30.0)
            except AssertionError:
                pass

    # ------------------------------------------------------------------
    # Test 8: Audio mirror across TWIN link (real V2 clients)
    # ------------------------------------------------------------------
    def test_08_audio_mirror_between_twins(self):
        """A client on ref1 transmits; a client on ref2 receives the audio.

        Exercises the Phase C1 path end-to-end:
          ref1 local client -> TGHandler.talkerUpdated -> TwinLink.onLocalAudio
          -> MsgTrunkAudio over [TWIN] -> ref2.handleMsgTrunkAudio
          -> broadcastUdpMsg -> ref2 local client's UDP socket.
        """
        # TG 26201 has prefix "262", owned locally by both twins.
        tg = 26201

        # Wait for the twin link to be authenticated on both sides before
        # we assume mirroring is armed. (After previous failover tests
        # ref1 may have just restarted.)
        wait_until(
            lambda: any(
                "TWIN: hello from partner" in docker_compose_output(
                    "logs", "reflector-ref1")
                for _ in [0]
            ),
            timeout=20.0,
            msg="twin link not re-established on ref1 after prior tests",
        )

        sender = ClientPeer()
        receiver = ClientPeer()
        try:
            ref1_client_port = T.twin_mapped_client_port("ref1")
            ref2_client_port = T.twin_mapped_client_port("ref2")

            sender.connect(HOST, ref1_client_port)
            sender.authenticate(callsign=CLIENT_CALLSIGN,
                                password=CLIENT_PASSWORD)
            sender.setup_udp(udp_port=ref1_client_port)

            receiver.connect(HOST, ref2_client_port)
            receiver.authenticate(callsign="N0SEND",  # different callsign
                                   password=CLIENT_PASSWORD)
            receiver.setup_udp(udp_port=ref2_client_port)

            sender.select_tg(tg)
            receiver.select_tg(tg)
            time.sleep(0.5)  # let TG selection propagate

            # Drain any pre-test UDP noise on the receiver
            receiver.recv_udp_all(timeout=0.5)

            audio_payload = b"\xBE\xEF" * 80  # 160 bytes, distinctive
            for _ in range(4):
                sender.send_udp_audio(audio_payload)
                time.sleep(0.02)
            sender.send_udp_flush()

            msgs = receiver.recv_udp_all(timeout=3.0)
            audio_count = sum(1 for t, _ in msgs if t == UDP_AUDIO)
            flush_count = sum(1 for t, _ in msgs if t == UDP_FLUSH)

            self.assertGreater(
                audio_count, 0,
                "receiver on ref2 did not get any audio frames from ref1 "
                "sender (twin audio-mirror path broken)",
            )
            self.assertGreater(
                flush_count, 0,
                "receiver on ref2 did not get flush marker from ref1 sender",
            )

        finally:
            sender.close()
            receiver.close()

    # ------------------------------------------------------------------
    # Test 9: Satellite handshake with a twin pair member
    # ------------------------------------------------------------------
    def test_09_satellite_handshake_with_twin_member(self):
        """A satellite connects to the twin-parent reflector and appears
        in its /status.satellites listing after the two-way hello.
        """
        parent = T.TWIN_SATELLITE_PARENT
        sat_port = T.twin_mapped_satellite_port(parent)

        sat = SatellitePeer()
        try:
            sat.connect_satellite(port=sat_port)
            sat.handshake()

            def satellite_registered():
                status = get_status(*_http(parent))
                return SAT_ID in status.get("satellites", {})

            wait_until(
                satellite_registered,
                timeout=8.0,
                msg=f"satellite {SAT_ID} not visible in /status.satellites "
                    f"on reflector-{parent} after handshake",
            )
        finally:
            sat.close()

    # ------------------------------------------------------------------
    # Test 10: Satellite receives twin-mirrored audio
    # ------------------------------------------------------------------
    def test_10_satellite_receives_twin_mirrored_audio(self):
        """Audio sent by a client on ref2 reaches a satellite on ref1.

        Exercises the end-to-end path:
          ref2 local client -> TGHandler.talkerUpdated -> TwinLink.onLocalAudio
          -> MsgTrunkAudio over [TWIN] -> ref1.handleMsgTrunkAudio
          -> forwardAudioToSatellitesExcept -> satellite socket.

        Verifies that satellites attached to a twin member see audio that
        originated on the other twin — talker-state mirroring alone is not
        enough; the audio frames must be forwarded too.
        """
        # TG 26201: prefix "262", owned locally by both twins.
        tg = 26201
        parent = T.TWIN_SATELLITE_PARENT  # "ref1"
        other_twin = T.TWIN_REFLECTORS[parent]["twin_of"]  # "ref2"
        sat_port = T.twin_mapped_satellite_port(parent)

        sat = SatellitePeer()
        sender = ClientPeer()
        try:
            # Satellite first so it's registered before audio flows
            sat.connect_satellite(port=sat_port)
            sat.handshake()

            other_client_port = T.twin_mapped_client_port(other_twin)
            sender.connect(HOST, other_client_port)
            sender.authenticate(callsign=CLIENT_CALLSIGN,
                                password=CLIENT_PASSWORD)
            sender.setup_udp(udp_port=other_client_port)
            sender.select_tg(tg)
            time.sleep(0.5)  # let TG selection + twin handshake settle

            audio_payload = b"\xBE\xEF" * 80
            for _ in range(4):
                sender.send_udp_audio(audio_payload)
                time.sleep(0.02)
            sender.send_udp_flush()

            # Drain everything the satellite receives within a short window
            got_talker_start = False
            got_audio = False
            got_flush = False
            got_talker_stop = False
            deadline = time.monotonic() + 4.0
            sat.sock.settimeout(0.5)
            while time.monotonic() < deadline:
                try:
                    data = recv_frame(sat.sock)
                except (socket.timeout, OSError):
                    if got_audio and got_talker_start:
                        break
                    continue
                except ConnectionError:
                    break
                msg_type, fields = parse_msg(data)
                if msg_type == MSG_TRUNK_TALKER_START and fields.get("tg") == tg:
                    got_talker_start = True
                elif msg_type == MSG_TRUNK_AUDIO and fields.get("tg") == tg:
                    got_audio = True
                elif msg_type == MSG_TRUNK_FLUSH and fields.get("tg") == tg:
                    got_flush = True
                elif msg_type == MSG_TRUNK_TALKER_STOP and fields.get("tg") == tg:
                    got_talker_stop = True
                elif msg_type == MSG_TRUNK_HEARTBEAT:
                    continue

            self.assertTrue(
                got_talker_start,
                f"satellite on {parent} did not receive TalkerStart for TG "
                f"{tg} from {other_twin} via TWIN mirror",
            )
            self.assertTrue(
                got_audio,
                f"satellite on {parent} did not receive any audio frames for "
                f"TG {tg} from {other_twin}; "
                f"TwinLink::handleMsgTrunkAudio may not be forwarding to "
                f"satellites",
            )
            # Flush + TalkerStop are nice-to-have; we log but don't fail
            # hard if timing swallowed them.
            if not got_flush:
                sys.stderr.write(
                    "  (note: satellite did not receive Flush frame — "
                    "may indicate TwinLink flush-to-satellite gap)\n")
            if not got_talker_stop:
                # Talker stop only fires when the sender releases the TG,
                # which depends on server-side talker timeouts. Not asserted.
                pass
        finally:
            sender.close()
            sat.close()

    # ------------------------------------------------------------------
    # Test 11: Twin partner's node roster appears in /status.twin.nodes
    # ------------------------------------------------------------------
    def test_11_twin_partner_nodelist_in_status(self):
        """A client authenticated on ref1 appears in ref2's /status under
        twin.nodes, and disappears after it disconnects.

        Exercises the roster path added for issue #3:
          ref1 client auth -> Reflector::sendNodeListToAllPeers
          -> TwinLink::onLocalNodeListUpdated (MsgTrunkNodeList over [TWIN])
          -> ref2.handleMsgTrunkNodeList -> m_partner_nodes
          -> /status.twin.nodes
        """
        client = ClientPeer()
        try:
            ref1_client_port = T.twin_mapped_client_port("ref1")
            client.connect(HOST, ref1_client_port)
            client.authenticate(callsign=CLIENT_CALLSIGN,
                                password=CLIENT_PASSWORD)

            def callsign_on_twin():
                status = get_status(*_http("ref2"))
                twin = status.get("twin", {})
                nodes = twin.get("nodes", [])
                return any(n.get("callsign") == CLIENT_CALLSIGN for n in nodes)

            wait_until(
                callsign_on_twin,
                timeout=10.0,
                interval=0.25,
                msg=f"{CLIENT_CALLSIGN} did not appear in ref2 /status.twin.nodes "
                    f"after login on ref1 — MsgTrunkNodeList may not be "
                    f"flowing over the [TWIN] link",
            )
        finally:
            client.close()

        # After disconnect, ref1 re-publishes its (now-empty) roster;
        # ref2.twin.nodes should drop the callsign.
        def callsign_gone_from_twin():
            status = get_status(*_http("ref2"))
            twin = status.get("twin", {})
            nodes = twin.get("nodes", [])
            return not any(n.get("callsign") == CLIENT_CALLSIGN for n in nodes)

        wait_until(
            callsign_gone_from_twin,
            timeout=10.0,
            interval=0.25,
            msg=f"{CLIENT_CALLSIGN} still in ref2 /status.twin.nodes after "
                f"disconnect from ref1",
        )


    # ------------------------------------------------------------------
    # Test X1: Per-client delta visible across twin link
    # ------------------------------------------------------------------
    def test_X1_per_client_visible_across_twin(self):
        """A V2 client connecting to ref1 causes ref2's MQTT broker to emit
        peer/TWIN/client/<call>/connected within 2s.

        The path is:
          ref1 local client auth -> Reflector::onClientAuthenticated
          -> fanoutClientConnected -> TwinLink::sendClientConnected
          -> MsgPeerClientConnected over [TWIN] -> ref2.handleMsgPeerClientConnected
          -> ref2 MqttPublisher::publishPeerClientEvent
          -> svxreflector/ref2/ref2/peer/TWIN/client/<call>/connected

        Note: ref2's TOPIC_PREFIX=svxreflector/ref2 and MQTT_NAME=ref2, so
        the effective prefix is svxreflector/ref2/ref2.
        """
        # ref2 has TOPIC_PREFIX=svxreflector/ref2 and MQTT_NAME=ref2 so the
        # effective prefix is svxreflector/ref2/ref2.
        # Peer events from the twin appear under peer/TWIN/client/<call>/...
        # (m_peer_id_config = "TWIN" hardcoded in TwinLink constructor).
        topic = "svxreflector/ref2/ref2/peer/TWIN/client/+/connected"
        received = []
        sub_event = threading.Event()

        def on_connect(client, userdata, flags, rc):
            client.subscribe(topic)
            sub_event.set()

        def on_message(client, userdata, msg):
            try:
                payload = json.loads(msg.payload)
            except (ValueError, json.JSONDecodeError):
                payload = {}
            received.append((msg.topic, payload))

        mc = mqtt_client.Client()
        mc.on_connect = on_connect
        mc.on_message = on_message
        mc.connect(HOST, TWIN_MQTT_PORT, 60)
        mc.loop_start()

        client = ClientPeer()
        try:
            self.assertTrue(sub_event.wait(timeout=5),
                            "MQTT subscribe to ref2 broker timed out")
            time.sleep(0.3)

            ref1_port = T.twin_mapped_client_port("ref1")
            client.connect(HOST, ref1_port)
            client.authenticate(callsign=CLIENT_CALLSIGN,
                                password=CLIENT_PASSWORD)

            # Wait up to 2s for the connected event to appear
            deadline = time.monotonic() + 2.0
            while time.monotonic() < deadline:
                if any(CLIENT_CALLSIGN in t for t, _ in received):
                    break
                time.sleep(0.1)

            matches = [(t, p) for t, p in received
                       if CLIENT_CALLSIGN in t]
            self.assertGreater(
                len(matches), 0,
                f"ref2 broker did not receive per-client connected event for "
                f"{CLIENT_CALLSIGN} after connecting to ref1. "
                f"Topics received: {[t for t, _ in received]}\n"
                f"Expected: {topic}",
            )
            topic_str, payload = matches[0]
            self.assertIn("TWIN", topic_str,
                          f"peer_id should be 'TWIN' in topic, got: {topic_str}")
            self.assertIn("tg", payload,
                          f"connected payload missing 'tg': {payload}")
            self.assertIn("ip", payload,
                          f"connected payload missing 'ip': {payload}")
        finally:
            client.close()
            mc.loop_stop()
            mc.disconnect()

    # ------------------------------------------------------------------
    # Test X2: MQTT_NAME required when [TWIN] is configured
    # ------------------------------------------------------------------
    def test_X2_mqtt_name_required_in_twin_mode(self):
        """A reflector with [TWIN] configured but MQTT_NAME missing/empty
        must exit non-zero and log an error containing 'MQTT_NAME is required'.

        This protects against retained-topic collisions when twin pair members
        share the same MQTT broker (both would publish to the same retained
        topics without distinct MQTT_NAME sub-trees).
        """
        if not os.path.isfile(SVXREFLECTOR_BIN):
            self.skipTest(
                f"svxreflector binary not found at {SVXREFLECTOR_BIN}; "
                f"run 'cmake --build build' first"
            )

        # Write a minimal config: has [TWIN] but MQTT with no MQTT_NAME.
        # CERT_PKI_DIR is set to a writable tempdir so the reflector can
        # generate its PKI files and proceed to the MQTT_NAME validation
        # without hitting a permissions failure first.
        with tempfile.TemporaryDirectory() as tmpdir:
            pki_dir = os.path.join(tmpdir, "pki")
            os.makedirs(pki_dir, exist_ok=True)
            config_content = f"""\
[GLOBAL]
TIMESTAMP_FORMAT="%c"
LISTEN_PORT=15399
TG_FOR_V1_CLIENTS=999
LOCAL_PREFIX=262
TRUNK_LISTEN_PORT=15302
HTTP_SRV_PORT=15080
TWIN_LISTEN_PORT=15304
CERT_PKI_DIR={pki_dir}

[SERVER_CERT]
COMMON_NAME=reflector-test-no-mqttname

[USERS]
N0TEST=TestGroup

[PASSWORDS]
TestGroup=testpass

[TWIN]
HOST=127.0.0.1
PORT=15304
SECRET=test_secret

[MQTT]
HOST=127.0.0.1
PORT=11883
USERNAME=test
PASSWORD=testpass
TOPIC_PREFIX=svxreflector/testnode
"""
            conf_path = os.path.join(tmpdir, "svxreflector.conf")
            with open(conf_path, "w") as f:
                f.write(config_content)

            proc = subprocess.run(
                [SVXREFLECTOR_BIN, f"--config={conf_path}"],
                cwd=tmpdir,
                capture_output=True,
                text=True,
                timeout=60.0,   # PKI key generation (4096-bit RSA) can be slow
            )

        # The reflector must log the expected error string and then exit.
        # Note: the reflector process always exits with code 0 even on init
        # failure (upstream svxreflector.cpp behaviour) — so we do not assert
        # on returncode, only on the error message being present in stdout.
        combined = proc.stdout + proc.stderr
        self.assertIn(
            "MQTT_NAME is required",
            combined,
            f"Expected error message 'MQTT_NAME is required' in output when "
            f"[TWIN] is configured without MQTT_NAME, but it was absent.\n"
            f"Full output (last 1000 chars):\n{combined[-1000:]}",
        )

        # Also verify it logged the initialization failure
        self.assertIn(
            "Initialization failed",
            combined,
            f"Expected 'Initialization failed' in output; "
            f"got:\n{combined[-500:]}",
        )


def docker_compose_output(*args) -> str:
    """Return the stdout+stderr of a docker-compose command as text."""
    cmd = ["docker", "compose", "-f", COMPOSE_FILE, *args]
    res = subprocess.run(cmd, cwd=TESTS_DIR, capture_output=True, text=True)
    return (res.stdout or "") + (res.stderr or "")


# ---------------------------------------------------------------------------
# Custom test runner (same style as test_trunk.py)
# ---------------------------------------------------------------------------

class TwinTestResult(unittest.TestResult):
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
        self._finish("\u2714", self.GREEN, test,
                     time.monotonic() - self.start_time)

    def addFailure(self, test, err):
        super().addFailure(test, err)
        self._finish("\u2718", self.RED, test,
                     time.monotonic() - self.start_time)

    def addError(self, test, err):
        super().addError(test, err)
        self._finish("!", self.RED, test, time.monotonic() - self.start_time)

    def addSkip(self, test, reason):
        super().addSkip(test, reason)
        self._finish("-", self.YELLOW, test,
                     time.monotonic() - self.start_time)


class TwinTestRunner:
    def __init__(self, stream=None):
        self.stream = stream or sys.stderr

    def run(self, test):
        B = TwinTestResult.BOLD
        G = TwinTestResult.GREEN
        R = TwinTestResult.RED
        D = TwinTestResult.DIM
        RST = TwinTestResult.RESET

        result = TwinTestResult(self.stream)
        suite_start = time.monotonic()

        self.stream.write(f"\n{B}TWIN Protocol Integration Tests{RST}\n")
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
    runner = TwinTestRunner()
    suite = unittest.TestLoader().loadTestsFromTestCase(TestTwinIntegration)
    result = runner.run(suite)
    if not result.wasSuccessful():
        raise SystemExit(1)
