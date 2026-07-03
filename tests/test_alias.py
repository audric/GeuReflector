#!/usr/bin/env python3
"""Integration tests for the local-alias feature (LOCAL_ALIASES).

A "local alias" re-homes a short local TG (e.g. `8`) under the reflector's
own owned prefix as a long, globally-routable number (e.g. `1220008`) so it
routes internationally like any owned TG, while local users keep dialing the
short number. See docs/PEER_PROTOCOL.md "Local aliases" for the full spec.

Test topology: reflector `a` (default mesh, prefix `122`) is configured with

    LOCAL_ALIASES=8:1220008,90:1220090/out,91:1220091/in

  - 8  <-> 1220008  two-way (local talk exported to mesh AND foreign talk
                     heard locally)
  - 90 <-> 1220090  /out    (export only: a's TG 90 reaches the mesh; the
                     mesh's 1220090 is NOT delivered to a's local TG 90)
  - 91 <-> 1220091  /in     (import only: mesh's 1220091 is delivered to a's
                     local TG 91; a's local TG 91 is NOT published outward)

Reflector `b` (prefix `121`) trunks directly to `a` and plays the role of
"peer abroad" whose clients dial the long numbers directly (b has no alias
table of its own — the raw long number IS the identity there).

Reuses ClientPeer / get_status / wait_until / wait_for_reflector /
wait_for_trunk_connected from test_trunk.py — same harness, same assertion
style. Requires: Python 3.7+ (stdlib only) + topology.py + test_trunk.py
from this directory.
"""

import os
import subprocess
import sys
import tempfile
import time
import unittest
from urllib.error import URLError

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import topology as T
from test_trunk import (  # noqa: E402
    ClientPeer, TrunkPeer, UDP_AUDIO, UDP_FLUSH,
    CLIENT_CALLSIGN, CLIENT_PASSWORD,
    CLIENT2_CALLSIGN, CLIENT2_PASSWORD,
    CLIENT3_CALLSIGN, CLIENT3_PASSWORD,
    HOST, REFLECTOR_NAMES,
    get_status, wait_until, wait_for_reflector, wait_for_trunk_connected,
)

# ---------------------------------------------------------------------------
# Topology constants
# ---------------------------------------------------------------------------

ALIAS_OWNER = "a"   # reflector carrying LOCAL_ALIASES (prefix 122)
ALIAS_PEER = "b"    # peer reflector trunked directly to ALIAS_OWNER

# Alias table configured on reflector 'a' (must match topology.py verbatim)
TG_TWOWAY_SHORT, TG_TWOWAY_LONG = 8, 1220008
TG_OUT_SHORT, TG_OUT_LONG = 90, 1220090
TG_IN_SHORT, TG_IN_LONG = 91, 1220091

EXPECTED_ALIASES = {
    str(TG_TWOWAY_SHORT): {"alias": TG_TWOWAY_LONG, "direction": "two-way"},
    str(TG_OUT_SHORT):    {"alias": TG_OUT_LONG,    "direction": "out"},
    str(TG_IN_SHORT):     {"alias": TG_IN_LONG,     "direction": "in"},
}

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
SVXREFLECTOR_BIN = os.path.join(
    os.path.dirname(TESTS_DIR), "build", "bin", "svxreflector")


def _http(name: str):
    return (HOST, T.mapped_http_port(name))


def _client_port(name: str):
    return T.mapped_client_port(name)


def _peer_roster_tg(peer_name: str, owner_name: str, callsign: str):
    """Return the TG a `peer_name` reflector sees for `callsign` in the
    owner reflector's trunk roster (`/status.trunks[<A_B>].nodes`), or None
    if the callsign is not (yet) present.

    This is the observable used to verify the node's effective TG WITHOUT
    the client having to send a MsgNodeInfo blob (the harness ClientPeer
    never does): the per-client trunk node-list is emitted on client
    connect/select and carries the reflector-side current TG — which is the
    *folded* (short) number after rewrite-on-select. Same signal test_27b
    in test_trunk.py relies on.
    """
    section = T.trunk_section_name(owner_name, peer_name)
    status = get_status(*_http(peer_name))
    nodes = status.get("trunks", {}).get(section, {}).get("nodes", [])
    for n in nodes:
        if n.get("callsign") == callsign:
            return n.get("tg")
    return None


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

class TestLocalAlias(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        """Wait for the default mesh to be up with a<->b trunked."""
        D = "\033[2m"
        G = "\033[32m"
        RST = "\033[0m"

        sys.stderr.write(f"  {D}waiting for reflectors...{RST}")
        sys.stderr.flush()
        for name in REFLECTOR_NAMES:
            wait_for_reflector(*_http(name), timeout=90.0)
        sys.stderr.write(f"\r\033[K  {G}✔{RST} Reflectors up\n")

        sys.stderr.write(f"  {D}waiting for mesh...{RST}")
        sys.stderr.flush()
        for name in REFLECTOR_NAMES:
            for peer in REFLECTOR_NAMES:
                if peer != name and T.should_trunk(name, peer):
                    wait_for_trunk_connected(
                        *_http(name), T.trunk_section_name(name, peer),
                        timeout=30.0)
        sys.stderr.write(f"\r\033[K  {G}✔{RST} Mesh connected\n")
        sys.stderr.write(f"\033[2m{'-' * 50}\033[0m\n")

    # ------------------------------------------------------------------
    # Test 01: two-way alias — plain local same-TG delivery is unaffected
    # ------------------------------------------------------------------
    def test_01_two_way_local_delivery(self):
        """A local same-TG conversation on the short number 8 still works
        exactly like any normal TG once an alias table is configured —
        both nodes select the short number, and both nodes are tracked
        reflector-side on TG 8 (observed via the trunk-peer roster, which
        carries each client's effective reflector-side TG)."""
        node1 = ClientPeer()
        node2 = ClientPeer()
        try:
            port = _client_port(ALIAS_OWNER)
            node1.connect(HOST, port)
            node1.authenticate(callsign=CLIENT_CALLSIGN, password=CLIENT_PASSWORD)
            node1.setup_udp(udp_port=port)
            node1.select_tg(TG_TWOWAY_SHORT)

            node2.connect(HOST, port)
            node2.authenticate(callsign=CLIENT2_CALLSIGN, password=CLIENT2_PASSWORD)
            node2.setup_udp(udp_port=port)
            node2.select_tg(TG_TWOWAY_SHORT)

            time.sleep(0.5)
            node2.recv_udp_all(timeout=0.05)  # drain bootstrap traffic

            for _ in range(6):
                node1.send_udp_audio(b"\xAB\xCD" * 80)
                time.sleep(0.02)
            node1.send_udp_flush()
            time.sleep(0.8)

            msgs = node2.recv_udp_all(timeout=1.0)
            audio = sum(1 for t, _ in msgs if t == UDP_AUDIO)
            flush = sum(1 for t, _ in msgs if t == UDP_FLUSH)
            self.assertGreater(audio + flush, 0,
                "node2 (short TG 8) received no audio from node1 — plain "
                "same-reflector delivery broken by the presence of an "
                "alias table")

            # Both clients are tracked on the short TG 8 reflector-side.
            for cs in (CLIENT_CALLSIGN, CLIENT2_CALLSIGN):
                wait_until(
                    lambda cs=cs: _peer_roster_tg(ALIAS_PEER, ALIAS_OWNER, cs)
                    == TG_TWOWAY_SHORT,
                    timeout=8.0,
                    msg=f"{cs} not tracked on TG {TG_TWOWAY_SHORT} in "
                        f"reflector-{ALIAS_PEER}'s view of reflector-"
                        f"{ALIAS_OWNER}'s roster")
        finally:
            node1.close()
            node2.close()

    # ------------------------------------------------------------------
    # Test 02: two-way alias — local talk on the short TG reaches a peer
    # ------------------------------------------------------------------
    def test_02_two_way_reaches_peer(self):
        """A-node keys the short number 8; a B-node that expressed interest
        in the long number 1220008 must hear it (published to the mesh
        under the owned long number)."""
        a_node = ClientPeer()
        b_node = ClientPeer()
        try:
            b_port = _client_port(ALIAS_PEER)
            b_node.connect(HOST, b_port)
            b_node.authenticate(callsign=CLIENT_CALLSIGN, password=CLIENT_PASSWORD)
            b_node.setup_udp(udp_port=b_port)
            b_node.select_tg(TG_TWOWAY_LONG)  # expresses interest on B

            a_port = _client_port(ALIAS_OWNER)
            a_node.connect(HOST, a_port)
            a_node.authenticate(callsign=CLIENT2_CALLSIGN, password=CLIENT2_PASSWORD)
            a_node.setup_udp(udp_port=a_port)
            a_node.select_tg(TG_TWOWAY_SHORT)

            # Let B's TG-interest advertisement (500 ms debounce) reach A.
            time.sleep(1.2)
            b_node.recv_udp_all(timeout=0.05)

            for _ in range(6):
                a_node.send_udp_audio(b"\xBE\xEF" * 80)
                time.sleep(0.02)
            a_node.send_udp_flush()
            time.sleep(1.2)

            msgs = b_node.recv_udp_all(timeout=1.0)
            audio = sum(1 for t, _ in msgs if t == UDP_AUDIO)
            flush = sum(1 for t, _ in msgs if t == UDP_FLUSH)
            self.assertGreater(audio + flush, 0,
                f"B-node (selected long TG {TG_TWOWAY_LONG}) received no "
                f"audio from A's short TG {TG_TWOWAY_SHORT} — outbound "
                f"alias publish (aliasToRoute) broken")
        finally:
            a_node.close()
            b_node.close()

    # ------------------------------------------------------------------
    # Test 03: two-way alias — inbound long-number traffic folds to local
    # ------------------------------------------------------------------
    def test_03_two_way_inbound_to_local(self):
        """B-node keys the long number 1220008 directly; an A-node
        monitoring the short number 8 must hear it (inbound delivery folds
        the owned long number back onto the local short number)."""
        a_node = ClientPeer()
        b_node = ClientPeer()
        try:
            a_port = _client_port(ALIAS_OWNER)
            a_node.connect(HOST, a_port)
            a_node.authenticate(callsign=CLIENT_CALLSIGN, password=CLIENT_PASSWORD)
            a_node.setup_udp(udp_port=a_port)
            a_node.select_tg(0)
            a_node.monitor_tgs([TG_TWOWAY_SHORT])

            b_port = _client_port(ALIAS_PEER)
            b_node.connect(HOST, b_port)
            b_node.authenticate(callsign=CLIENT2_CALLSIGN, password=CLIENT2_PASSWORD)
            b_node.setup_udp(udp_port=b_port)
            b_node.select_tg(TG_TWOWAY_LONG)

            time.sleep(0.8)
            a_node.recv_udp_all(timeout=0.05)

            for _ in range(6):
                b_node.send_udp_audio(b"\xCA\xFE" * 80)
                time.sleep(0.02)
            b_node.send_udp_flush()
            time.sleep(1.2)

            msgs = a_node.recv_udp_all(timeout=1.0)
            audio = sum(1 for t, _ in msgs if t == UDP_AUDIO)
            flush = sum(1 for t, _ in msgs if t == UDP_FLUSH)
            self.assertGreater(audio + flush, 0,
                f"A-node monitoring short TG {TG_TWOWAY_SHORT} received no "
                f"audio from B's long TG {TG_TWOWAY_LONG} — inbound alias "
                f"delivery (aliasToLocal) broken")
        finally:
            a_node.close()
            b_node.close()

    # ------------------------------------------------------------------
    # Test 04: /out alias — publishes outbound, does not accept inbound
    # ------------------------------------------------------------------
    def test_04_out_only_no_inbound(self):
        """Alias 90:1220090/out. Outbound (A's short TG 90 -> mesh) still
        works; inbound (mesh's long TG 1220090 -> A's short TG 90) must
        NOT be delivered locally."""
        a_sender = ClientPeer()
        a_monitor = ClientPeer()
        b_node = ClientPeer()
        try:
            b_port = _client_port(ALIAS_PEER)
            b_node.connect(HOST, b_port)
            b_node.authenticate(callsign=CLIENT_CALLSIGN, password=CLIENT_PASSWORD)
            b_node.setup_udp(udp_port=b_port)
            b_node.select_tg(TG_OUT_LONG)  # primes B's interest for the leg below

            a_port = _client_port(ALIAS_OWNER)
            a_sender.connect(HOST, a_port)
            a_sender.authenticate(callsign=CLIENT2_CALLSIGN, password=CLIENT2_PASSWORD)
            a_sender.setup_udp(udp_port=a_port)
            a_sender.select_tg(TG_OUT_SHORT)

            a_monitor.connect(HOST, a_port)
            a_monitor.authenticate(callsign=CLIENT3_CALLSIGN, password=CLIENT3_PASSWORD)
            a_monitor.setup_udp(udp_port=a_port)
            a_monitor.select_tg(0)
            a_monitor.monitor_tgs([TG_OUT_SHORT])

            time.sleep(1.2)  # let B's interest advertisement land at A

            # --- Outbound still publishes: A's short TG 90 -> B ---
            b_node.recv_udp_all(timeout=0.05)
            for _ in range(6):
                a_sender.send_udp_audio(b"\x11\x22" * 80)
                time.sleep(0.02)
            a_sender.send_udp_flush()
            time.sleep(1.2)

            out_msgs = b_node.recv_udp_all(timeout=1.0)
            out_audio = sum(1 for t, _ in out_msgs if t == UDP_AUDIO)
            out_flush = sum(1 for t, _ in out_msgs if t == UDP_FLUSH)
            self.assertGreater(out_audio + out_flush, 0,
                f"B-node received no audio for /out alias TG {TG_OUT_LONG} "
                f"— outbound publish of a /out alias must still work")

            # --- Inbound must NOT be delivered: B's long TG -> A monitor ---
            a_monitor.recv_udp_all(timeout=0.05)
            for _ in range(6):
                b_node.send_udp_audio(b"\x33\x44" * 80)
                time.sleep(0.02)
            b_node.send_udp_flush()
            time.sleep(1.2)

            in_msgs = a_monitor.recv_udp_all(timeout=1.0)
            in_audio = sum(1 for t, _ in in_msgs if t == UDP_AUDIO)
            in_flush = sum(1 for t, _ in in_msgs if t == UDP_FLUSH)
            self.assertEqual(in_audio + in_flush, 0,
                f"A-node monitoring short TG {TG_OUT_SHORT} received "
                f"{in_audio} audio / {in_flush} flush frames from the mesh "
                f"— a /out-only alias must not deliver inbound traffic "
                f"locally")
        finally:
            a_sender.close()
            a_monitor.close()
            b_node.close()

    # ------------------------------------------------------------------
    # Test 05: /in alias — folds inbound, does not publish outbound
    # ------------------------------------------------------------------
    def test_05_in_only_no_publish(self):
        """Alias 91:1220091/in. Outbound (A's short TG 91 -> mesh) must NOT
        be published; inbound (mesh's long TG 1220091 -> A's short TG 91)
        still folds and is delivered locally."""
        a_node = ClientPeer()
        b_monitor = ClientPeer()
        b_node = ClientPeer()
        try:
            a_port = _client_port(ALIAS_OWNER)
            a_node.connect(HOST, a_port)
            a_node.authenticate(callsign=CLIENT_CALLSIGN, password=CLIENT_PASSWORD)
            a_node.setup_udp(udp_port=a_port)
            a_node.select_tg(TG_IN_SHORT)

            b_port = _client_port(ALIAS_PEER)
            b_monitor.connect(HOST, b_port)
            b_monitor.authenticate(callsign=CLIENT2_CALLSIGN, password=CLIENT2_PASSWORD)
            b_monitor.setup_udp(udp_port=b_port)
            b_monitor.select_tg(0)
            b_monitor.monitor_tgs([TG_IN_LONG])

            time.sleep(0.8)

            # --- Outbound must NOT publish: A's short TG 91 -> mesh ---
            b_monitor.recv_udp_all(timeout=0.05)
            for _ in range(6):
                a_node.send_udp_audio(b"\x55\x66" * 80)
                time.sleep(0.02)
            a_node.send_udp_flush()
            time.sleep(1.2)

            out_msgs = b_monitor.recv_udp_all(timeout=1.0)
            out_audio = sum(1 for t, _ in out_msgs if t == UDP_AUDIO)
            out_flush = sum(1 for t, _ in out_msgs if t == UDP_FLUSH)
            # NOTE: TG 91 is a plain local-only number (not owned, not cluster,
            # not peer-shared), so absent the alias it is never exported anyway.
            # This assertion therefore guards that an `/in` alias adds NO outbound
            # route (m_alias_route is not populated for `/in`) — it does NOT prove
            # active suppression of an otherwise-shareable TG, which `/in` by design
            # does not do (cluster/shared publishing is independent of aliasing).
            self.assertEqual(out_audio + out_flush, 0,
                f"B-node monitoring long TG {TG_IN_LONG} received "
                f"{out_audio} audio / {out_flush} flush frames from A's "
                f"short TG {TG_IN_SHORT} — a /in-only alias must not "
                f"publish outbound traffic")

            # --- Inbound still folds: B's long TG -> A's short TG ---
            b_node.connect(HOST, b_port)
            b_node.authenticate(callsign=CLIENT3_CALLSIGN, password=CLIENT3_PASSWORD)
            b_node.setup_udp(udp_port=b_port)
            b_node.select_tg(TG_IN_LONG)
            time.sleep(0.5)

            a_node.recv_udp_all(timeout=0.05)
            for _ in range(6):
                b_node.send_udp_audio(b"\x77\x88" * 80)
                time.sleep(0.02)
            b_node.send_udp_flush()
            time.sleep(1.2)

            in_msgs = a_node.recv_udp_all(timeout=1.0)
            in_audio = sum(1 for t, _ in in_msgs if t == UDP_AUDIO)
            in_flush = sum(1 for t, _ in in_msgs if t == UDP_FLUSH)
            self.assertGreater(in_audio + in_flush, 0,
                f"A-node on short TG {TG_IN_SHORT} received no audio from "
                f"B's long TG {TG_IN_LONG} — inbound fold for a /in alias "
                f"is broken")
        finally:
            a_node.close()
            b_monitor.close()
            b_node.close()

    # ------------------------------------------------------------------
    # Test 06: rewrite-on-select — dialing the long number folds locally
    # ------------------------------------------------------------------
    def test_06_rewrite_on_select(self):
        """A client that selects the long alias number directly is folded
        onto the short number — the reflector tracks it on TG 8, not
        1220008 (observed via the trunk-peer roster, whose per-client TG is
        the reflector-side current TG after rewrite-on-select)."""
        node = ClientPeer()
        try:
            port = _client_port(ALIAS_OWNER)
            node.connect(HOST, port)
            node.authenticate(callsign=CLIENT_CALLSIGN, password=CLIENT_PASSWORD)
            node.setup_udp(udp_port=port)
            node.select_tg(TG_TWOWAY_LONG)  # dial the LONG number directly

            wait_until(
                lambda: _peer_roster_tg(ALIAS_PEER, ALIAS_OWNER,
                                        CLIENT_CALLSIGN) == TG_TWOWAY_SHORT,
                timeout=8.0,
                msg=f"{CLIENT_CALLSIGN} selecting long TG {TG_TWOWAY_LONG} "
                    f"was not folded onto short TG {TG_TWOWAY_SHORT} "
                    f"(reflector-{ALIAS_PEER}'s roster view of reflector-"
                    f"{ALIAS_OWNER})")
            # And it must NOT be tracked on the long number.
            self.assertNotEqual(
                _peer_roster_tg(ALIAS_PEER, ALIAS_OWNER, CLIENT_CALLSIGN),
                TG_TWOWAY_LONG,
                f"{CLIENT_CALLSIGN} is still tracked on the long TG "
                f"{TG_TWOWAY_LONG} — rewrite-on-select fold did not apply")
        finally:
            node.close()

    # ------------------------------------------------------------------
    # Test 07: /status exposes the alias table
    # ------------------------------------------------------------------
    def test_07_status_exposes_aliases(self):
        """/status.local_aliases matches the configured LOCAL_ALIASES table
        exactly: keyed by short number, with alias (long number) and
        direction (two-way/out/in)."""
        status = get_status(*_http(ALIAS_OWNER))
        aliases = status.get("local_aliases")
        self.assertIsNotNone(aliases, "/status is missing 'local_aliases'")
        self.assertEqual(aliases, EXPECTED_ALIASES,
            f"/status.local_aliases does not match the configured table.\n"
            f"expected: {EXPECTED_ALIASES}\ngot:      {aliases}")

    # ------------------------------------------------------------------
    # Test 08: an alias TG outside any LOCAL_PREFIX is rejected at load
    # ------------------------------------------------------------------
    def test_08_reject_unowned_alias(self):
        """A LOCAL_ALIASES entry whose alias number is not under any
        LOCAL_PREFIX is rejected at config load: absent from
        /status.local_aliases, a rejection line is logged, and the
        reflector still starts (other, valid entries are kept)."""
        if not os.path.isfile(SVXREFLECTOR_BIN):
            self.skipTest(
                f"svxreflector binary not found at {SVXREFLECTOR_BIN}; "
                f"run 'cmake --build build' first")

        with tempfile.TemporaryDirectory() as tmpdir:
            pki_dir = os.path.join(tmpdir, "pki")
            os.makedirs(pki_dir, exist_ok=True)
            http_port = 19080
            listen_port = 19300
            trunk_port = 19302

            # 8:1220008 is valid (under LOCAL_PREFIX=122); 7:9990007 is not
            # (9990007 does not start with 122) and must be rejected.
            config_content = f"""\
[GLOBAL]
TIMESTAMP_FORMAT="%c"
LISTEN_PORT={listen_port}
TG_FOR_V1_CLIENTS=999
LOCAL_PREFIX=122
LOCAL_ALIASES=8:1220008,7:9990007
TRUNK_LISTEN_PORT={trunk_port}
HTTP_SRV_PORT={http_port}
CERT_PKI_DIR={pki_dir}

[SERVER_CERT]
COMMON_NAME=reflector-test-alias-reject

[USERS]
N0TEST=TestGroup

[PASSWORDS]
TestGroup=testpass
"""
            conf_path = os.path.join(tmpdir, "svxreflector.conf")
            with open(conf_path, "w") as f:
                f.write(config_content)

            proc = subprocess.Popen(
                [SVXREFLECTOR_BIN, f"--config={conf_path}"],
                cwd=tmpdir,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            status = None
            try:
                status = self._wait_for_status(http_port, timeout=60.0)
            finally:
                proc.terminate()
                try:
                    out, _ = proc.communicate(timeout=10.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    out, _ = proc.communicate(timeout=5.0)

            self.assertIsNotNone(status,
                "reflector never reached a ready /status — cannot verify "
                "alias rejection")

            aliases = status.get("local_aliases", {})
            self.assertIn("8", aliases,
                "valid alias 8:1220008 is missing from /status.local_aliases "
                "— it should have been kept alongside the rejected entry")
            self.assertEqual(aliases["8"]["alias"], 1220008)
            self.assertNotIn("7", aliases,
                "unowned alias 7:9990007 should have been rejected and "
                "absent from /status.local_aliases")
            self.assertIn("not under any LOCAL_PREFIX", out,
                f"expected rejection log line not found in reflector "
                f"output; output tail:\n{out[-1000:]}")

    def test_08b_reject_duplicate_short_alias(self):
        """Two LOCAL_ALIASES entries sharing the same short (local) TG must
        keep only the FIRST mapping: the second is rejected at config load
        with a 'duplicate local TG' log line, and /status.local_aliases
        reflects the first entry's alias number."""
        if not os.path.isfile(SVXREFLECTOR_BIN):
            self.skipTest(
                f"svxreflector binary not found at {SVXREFLECTOR_BIN}; "
                f"run 'cmake --build build' first")

        with tempfile.TemporaryDirectory() as tmpdir:
            pki_dir = os.path.join(tmpdir, "pki")
            os.makedirs(pki_dir, exist_ok=True)
            http_port = 19081
            listen_port = 19301
            trunk_port = 19303

            # Both 1220008 and 1220009 are valid alias targets under
            # LOCAL_PREFIX=122, but they share the same short TG 8 — the
            # second entry must be rejected, keeping the first (1220008).
            config_content = f"""\
[GLOBAL]
TIMESTAMP_FORMAT="%c"
LISTEN_PORT={listen_port}
TG_FOR_V1_CLIENTS=999
LOCAL_PREFIX=122
LOCAL_ALIASES=8:1220008,8:1220009
TRUNK_LISTEN_PORT={trunk_port}
HTTP_SRV_PORT={http_port}
CERT_PKI_DIR={pki_dir}

[SERVER_CERT]
COMMON_NAME=reflector-test-alias-dup

[USERS]
N0TEST=TestGroup

[PASSWORDS]
TestGroup=testpass
"""
            conf_path = os.path.join(tmpdir, "svxreflector.conf")
            with open(conf_path, "w") as f:
                f.write(config_content)

            proc = subprocess.Popen(
                [SVXREFLECTOR_BIN, f"--config={conf_path}"],
                cwd=tmpdir,
                stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
            status = None
            try:
                status = self._wait_for_status(http_port, timeout=60.0)
            finally:
                proc.terminate()
                try:
                    out, _ = proc.communicate(timeout=10.0)
                except subprocess.TimeoutExpired:
                    proc.kill()
                    out, _ = proc.communicate(timeout=5.0)

            self.assertIsNotNone(status,
                "reflector never reached a ready /status — cannot verify "
                "duplicate-short-alias rejection")

            aliases = status.get("local_aliases", {})
            self.assertIn("8", aliases,
                "short TG 8 is missing from /status.local_aliases — the "
                "first mapping should have been kept")
            self.assertEqual(aliases["8"]["alias"], 1220008,
                "duplicate short TG 8 must keep the FIRST mapping "
                "(1220008), not the second (1220009)")
            self.assertIn("duplicate local TG", out,
                f"expected duplicate-rejection log line not found in "
                f"reflector output; output tail:\n{out[-1000:]}")

    @staticmethod
    def _wait_for_status(port, timeout=60.0):
        deadline = time.monotonic() + timeout
        last_exc = None
        while time.monotonic() < deadline:
            try:
                return get_status(HOST, port, timeout=2.0)
            except (URLError, OSError, ValueError) as e:
                last_exc = e
            time.sleep(0.5)
        raise AssertionError(
            f"reflector on port {port} never became ready "
            f"(last error: {last_exc})")

    # ------------------------------------------------------------------
    # Test 09: multi-hop delivery by number (skipped on this topology)
    # ------------------------------------------------------------------
    def test_09_multi_hop_by_number(self):
        """A C-node dialing the long alias number should hear an A-node on
        the short number through a gateway B, with no alias/TG_MAP config
        on B — proving the alias routes purely by number, hop by hop.

        The default mesh's only extra hop ('d', prefix 1219) is nested
        UNDER b's prefix ('121' is a prefix of '1219') specifically so
        gateway/peer-interest propagation works for TGs owned BY d (see
        test_37_gateway_prefix_routing / test_39_passive_monitor_multihop
        in test_trunk.py). Reflector 'a' (the alias owner here, prefix
        '122') has no such nesting relationship with b/d's prefixes: a
        d-side client selecting the long alias number (1220008) fails the
        isSharedTG gate on d's single TRUNK_B_D link (1220008 does not
        start with b's prefix '121'), so d never advertises interest in it
        upstream, and b never learns to relay a's aliased traffic onward.
        Reaching a true 3-hop scenario for an aliased TG would require
        additional ROUTABLE_PREFIXES plumbing on TRUNK_B_D that is out of
        scope for this alias-only topology change (adding it would also
        risk perturbing test_37/test_39, which depend on the current
        b<->d prefix relationship).
        """
        self.skipTest(
            "default topology's only 3rd hop ('d', prefix 1219) nests "
            "under b's prefix (121), not under a's owned prefix (122) "
            "that hosts the alias table; multi-hop-by-number for an "
            "aliased TG would require additional ROUTABLE_PREFIXES "
            "plumbing on TRUNK_B_D that is out of scope for this "
            "alias-only topology change")

    # ------------------------------------------------------------------
    # Test 10: mid-talk trunk drop must clear the aliased trunk-talker
    #          state (regression for TrunkLink::clearPeerTalkerState)
    # ------------------------------------------------------------------
    def test_10_disconnect_clears_aliased_trunk_talker(self):
        """Reproduces the bug fixed in TrunkLink::clearPeerTalkerState():
        that loop iterates m_peer_active_tgs, which holds LONG numbers,
        but handleMsgPeerTalkerStart records the trunk-talker in
        TGHandler under the SHORT (folded) number via
        aliasToLocal(local_tg). If clearPeerTalkerState clears the LONG
        key instead of folding it first, the SHORT TG's trunk-talker
        entry is never released after a trunk drop, and a local client
        can never win talker arbitration on it again — see
        TGHandler::hasTrunkTalker() gating local talker claims in
        Reflector::udpDatagramReceived.

        We use a fake trunk peer on the isolated TRUNK_TEST section
        (same harness facility as test_trunk.py; a black-hole outbound
        HOST so only our inbound connection is ever active) to avoid
        perturbing the real a<->b mesh link used by the rest of this
        suite. The fake peer claims the two-way alias's LONG number
        (1220008) as talker and is then dropped via a raw TCP close
        WITHOUT a TalkerStop — the closest achievable simulation of a
        trunk link failing mid-transmission with this harness (there is
        no supported way to kill only the data channel of a live Docker
        trunk link out from under a container).

        The only externally observable effect of hasTrunkTalker(8) is
        whether a local client's audio on the short number is ever
        accepted as talker and relayed; that is the real assertion here.
        """
        owner_http = _http(ALIAS_OWNER)
        fake_peer = TrunkPeer()
        talker_claimed = False
        try:
            fake_peer.connect(HOST, T.mapped_trunk_port(ALIAS_OWNER))
            fake_peer.handshake()  # TRUNK_TEST section, prefix 9, test_secret
            wait_for_trunk_connected(*owner_http, "TRUNK_TEST", timeout=10.0)

            fake_peer.send_talker_start(TG_TWOWAY_LONG, "N0FAKE")
            talker_claimed = True
            for _ in range(4):
                fake_peer.send_audio(TG_TWOWAY_LONG, b"\x99\x11" * 80)
                time.sleep(0.02)

            # Confirm the reflector actually recorded the claim (folded
            # onto the short number via deliver_tg) before we pull the
            # rug out — otherwise the recovery check below would be
            # vacuous (nothing was ever wedged to begin with).
            wait_until(
                lambda: get_status(*owner_http)["trunks"]["TRUNK_TEST"]
                    .get("active_talkers", {}).get(str(TG_TWOWAY_LONG))
                    == "N0FAKE",
                timeout=5.0,
                msg=f"fake peer's TalkerStart on long TG {TG_TWOWAY_LONG} "
                    f"never showed up in TRUNK_TEST's active_talkers — "
                    f"cannot exercise the disconnect-clears-state path")

            # Mid-talk drop: raw TCP close, no TalkerStop. This triggers
            # TrunkLink::onInboundDisconnected -> clearPeerTalkerState().
            fake_peer.close()

            wait_until(
                lambda: not get_status(*owner_http)["trunks"]["TRUNK_TEST"]
                    .get("connected", True),
                timeout=10.0,
                msg="TRUNK_TEST link on reflector-" + ALIAS_OWNER +
                    " did not register as disconnected after the fake "
                    "peer's TCP close")
        finally:
            if talker_claimed:
                fake_peer.close()  # no-op if already closed above

        # Recovery check: a local client on the SHORT number must now be
        # able to win talker arbitration and have its audio delivered to
        # another local client on the same short number. Before the fix,
        # clearPeerTalkerState left TG 8's trunk-talker entry wedged on
        # TGHandler forever, so hasTrunkTalker(8) stayed true and this
        # audio would never be accepted/relayed.
        node1 = ClientPeer()
        node2 = ClientPeer()
        try:
            port = _client_port(ALIAS_OWNER)
            node1.connect(HOST, port)
            node1.authenticate(callsign=CLIENT_CALLSIGN, password=CLIENT_PASSWORD)
            node1.setup_udp(udp_port=port)
            node1.select_tg(TG_TWOWAY_SHORT)

            node2.connect(HOST, port)
            node2.authenticate(callsign=CLIENT2_CALLSIGN, password=CLIENT2_PASSWORD)
            node2.setup_udp(udp_port=port)
            node2.select_tg(TG_TWOWAY_SHORT)

            time.sleep(0.5)
            node2.recv_udp_all(timeout=0.05)  # drain bootstrap traffic

            for _ in range(6):
                node1.send_udp_audio(b"\xDE\xAD" * 80)
                time.sleep(0.02)
            node1.send_udp_flush()
            time.sleep(0.8)

            msgs = node2.recv_udp_all(timeout=1.0)
            audio = sum(1 for t, _ in msgs if t == UDP_AUDIO)
            flush = sum(1 for t, _ in msgs if t == UDP_FLUSH)
            self.assertGreater(audio + flush, 0,
                f"node2 received no audio from node1 on short TG "
                f"{TG_TWOWAY_SHORT} after the fake trunk peer's mid-talk "
                f"drop — TG {TG_TWOWAY_SHORT}'s trunk-talker state is "
                f"still wedged, meaning clearPeerTalkerState() did not "
                f"fold the long alias number to the short number before "
                f"clearing TGHandler's trunk-talker map")
        finally:
            node1.close()
            node2.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
