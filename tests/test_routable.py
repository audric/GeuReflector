#!/usr/bin/env python3
"""Integration tests for GeuReflector routable-prefix transit feature.

Verifies that ROUTABLE_PREFIXES config allows a reflector to forward audio
toward a peer it does not own, enabling multi-hop transit across a chain.

Topology used: ROUTABLE (generate_configs.py --topology routable)
  - reflector-leaf   (LOCAL_PREFIX=222100) — leaf uplink, ROUTABLE_PREFIXES=* toward natlit
  - reflector-natlit (LOCAL_PREFIX=222)    — national Italy, ROUTABLE_PREFIXES=263 toward natlde
  - reflector-natlde (LOCAL_PREFIX=262)    — national DE, ROUTABLE_PREFIXES=263 toward far
  - reflector-far    (LOCAL_PREFIX=263)    — far peer that owns TG 263xx

Chain: leaf --TRUNK_LEAF_NATLIT--> natlit --TRUNK_NATLDE_NATLIT--> natlde --TRUNK_FAR_NATLDE--> far

Without ROUTABLE_PREFIXES the 263xx TG is unreachable from leaf (263 is not
owned by any hop until far). These tests assert the forward + return audio
paths once the feature is implemented.

test_01 uses TG 26305 (flows normally — no blacklist match).
test_02 uses TG 26307 (blacklisted on leaf's uplink — far must hear nothing).

Requires: Python 3.7+, stdlib only (+ topology.py from this directory).
Docker must be running and the ROUTABLE topology must have been generated:
    cd tests && python3 generate_configs.py --topology routable
    docker compose -f docker-compose.routable.yml up -d --build --wait
"""

import os
import sys
import time
import unittest
from urllib.request import urlopen
from urllib.error import URLError
import json

# Reuse the V2 mock client and wire helpers from test_trunk.py.
# Also ensures tests/ is on the path for topology import below.
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from test_trunk import (  # noqa: E402
    ClientPeer, UDP_AUDIO, UDP_FLUSH,
    CLIENT_CALLSIGN, CLIENT_PASSWORD,
    CLIENT2_CALLSIGN, CLIENT2_PASSWORD,
)

import topology as T

# ---------------------------------------------------------------------------
# Derived endpoints (routable topology)
# ---------------------------------------------------------------------------

HOST = "127.0.0.1"

ROUTABLE_NAMES = sorted(T.ROUTABLE_REFLECTORS)


def _http(name: str):
    return (HOST, T.routable_mapped_http_port(name))


def _client_port(name: str) -> int:
    return T.routable_mapped_client_port(name)


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


# ---------------------------------------------------------------------------
# Test class
# ---------------------------------------------------------------------------

class TestRoutableIntegration(unittest.TestCase):
    """Integration tests for the ROUTABLE_PREFIXES transit feature."""

    @classmethod
    def setUpClass(cls):
        """Wait for all ROUTABLE reflectors to be healthy and mesh to stabilise."""
        D = "\033[2m"
        G = "\033[32m"
        RST = "\033[0m"

        sys.stderr.write(f"  {D}waiting for routable reflectors...{RST}")
        sys.stderr.flush()
        for name in ROUTABLE_NAMES:
            wait_for_reflector(*_http(name), timeout=90.0)
        sys.stderr.write(f"\r\033[K  {G}✔{RST} Routable reflectors up\n")
        sys.stderr.flush()

        # Wait for trunk connections across the chain:
        #   leaf <--> natlit, natlit <--> natlde, natlde <--> far
        sys.stderr.write(f"  {D}waiting for trunk chain...{RST}")
        sys.stderr.flush()
        wait_for_trunk_connected(*_http("leaf"),   "TRUNK_LEAF_NATLIT",    timeout=30.0)
        wait_for_trunk_connected(*_http("natlit"), "TRUNK_LEAF_NATLIT",    timeout=30.0)
        wait_for_trunk_connected(*_http("natlit"), "TRUNK_NATLDE_NATLIT",  timeout=30.0)
        wait_for_trunk_connected(*_http("natlde"), "TRUNK_NATLDE_NATLIT",  timeout=30.0)
        wait_for_trunk_connected(*_http("natlde"), "TRUNK_FAR_NATLDE",     timeout=30.0)
        wait_for_trunk_connected(*_http("far"),    "TRUNK_FAR_NATLDE",     timeout=30.0)
        sys.stderr.write(f"\r\033[K  {G}✔{RST} Trunk chain connected\n")
        sys.stderr.flush()

        # Give interest-debounce timers a moment to settle
        time.sleep(1.0)

    # ------------------------------------------------------------------
    # Test 01: explicit transit — leaf client can reach far client (TG 26305)
    # ------------------------------------------------------------------
    def test_01_explicit_transit_forward_and_return(self):
        """ROUTABLE_PREFIXES=* on leaf and ROUTABLE_PREFIXES=263 on natlit/natlde
        must route TG 26305 all the way from leaf to far and back.

        leaf --[ROUTABLE_PREFIXES=*]--> natlit --[ROUTABLE_PREFIXES=263]--> natlde
             --[ROUTABLE_PREFIXES=263]--> far

        Forward leg: leaf client selects TG 26305, far client selects 26305.
          leaf sends 6 UDP audio frames + flush.
          far must receive > 0 audio or flush packets.

        Return leg: far sends 6 UDP audio frames + flush.
          leaf must receive > 0 audio or flush packets.

        NOTE: This test is expected to FAIL until ROUTABLE_PREFIXES is
        implemented in the C++ reflector (Task 1 TDD anchor).
        """
        tg = 26305  # owned by far (prefix 263); unreachable without transit

        leaf_port = _client_port("leaf")
        far_port  = _client_port("far")

        leaf_client = ClientPeer()
        far_client  = ClientPeer()
        try:
            # leaf client — selects TG 26305
            leaf_client.connect(HOST, leaf_port)
            leaf_client.authenticate(callsign=CLIENT_CALLSIGN,
                                     password=CLIENT_PASSWORD)
            leaf_client.setup_udp(udp_port=leaf_port)
            leaf_client.select_tg(tg)

            # far client — selects TG 26305 (owner side)
            far_client.connect(HOST, far_port)
            far_client.authenticate(callsign=CLIENT2_CALLSIGN,
                                    password=CLIENT2_PASSWORD)
            far_client.setup_udp(udp_port=far_port)
            far_client.select_tg(tg)

            # Allow peer-interest debounce (500 ms each hop × 2 hops) + margin
            time.sleep(2.0)

            # Drain any backlog before the measurement burst
            leaf_client.recv_udp_all(timeout=0.05)
            far_client.recv_udp_all(timeout=0.05)

            # --- Forward leg: leaf → far ---
            for _ in range(6):
                leaf_client.send_udp_audio(b"\xBE\xEF" * 80)
                time.sleep(0.02)
            leaf_client.send_udp_flush()
            time.sleep(1.5)

            fwd_msgs  = far_client.recv_udp_all(timeout=1.0)
            fwd_audio = sum(1 for t, _ in fwd_msgs if t == UDP_AUDIO)
            fwd_flush = sum(1 for t, _ in fwd_msgs if t == UDP_FLUSH)
            self.assertGreater(
                fwd_audio + fwd_flush, 0,
                f"forward leg FAILED: far client heard 0 UDP frames for TG {tg}. "
                f"ROUTABLE_PREFIXES transit is not yet implemented (expected failure).",
            )

            # --- Return leg: far → leaf ---
            leaf_client.recv_udp_all(timeout=0.05)  # flush stale
            for _ in range(6):
                far_client.send_udp_audio(b"\xCA\xFE" * 80)
                time.sleep(0.02)
            far_client.send_udp_flush()
            time.sleep(1.5)

            ret_msgs  = leaf_client.recv_udp_all(timeout=1.0)
            ret_audio = sum(1 for t, _ in ret_msgs if t == UDP_AUDIO)
            ret_flush = sum(1 for t, _ in ret_msgs if t == UDP_FLUSH)
            self.assertGreater(
                ret_audio + ret_flush, 0,
                f"return leg FAILED: leaf client heard 0 UDP frames for TG {tg}. "
                f"ROUTABLE_PREFIXES transit is not yet implemented (expected failure).",
            )

        finally:
            leaf_client.close()
            far_client.close()

    # ------------------------------------------------------------------
    # Test 02: wildcard + blacklist veto (TG 26307 blocked on leaf uplink)
    # ------------------------------------------------------------------
    def test_02_wildcard_blacklist_veto(self):
        """BLACKLIST_TGS=26307 on leaf's uplink must prevent TG 26307 from
        reaching far, even though ROUTABLE_PREFIXES=* would otherwise route it.

        leaf selects TG 26307, far selects 26307.
        leaf sends 6 UDP audio frames + flush.
        far must receive EXACTLY 0 audio or flush packets.

        This test is expected to PASS once ROUTABLE_PREFIXES is implemented,
        because the blacklist veto is a separate filter that suppresses the
        specific TG before routable transit logic fires.  Until the feature
        lands, far also hears nothing (transit not working), so the assertion
        holds trivially — but it documents the intended blacklist behaviour.
        """
        tg = 26307  # blacklisted on leaf → natlit link; far must hear nothing

        leaf_port = _client_port("leaf")
        far_port  = _client_port("far")

        leaf_client = ClientPeer()
        far_client  = ClientPeer()
        try:
            # leaf client — selects TG 26307
            leaf_client.connect(HOST, leaf_port)
            leaf_client.authenticate(callsign=CLIENT_CALLSIGN,
                                     password=CLIENT_PASSWORD)
            leaf_client.setup_udp(udp_port=leaf_port)
            leaf_client.select_tg(tg)

            # far client — selects TG 26307 (owner side)
            far_client.connect(HOST, far_port)
            far_client.authenticate(callsign=CLIENT2_CALLSIGN,
                                    password=CLIENT2_PASSWORD)
            far_client.setup_udp(udp_port=far_port)
            far_client.select_tg(tg)

            # Allow interest debounce to settle
            time.sleep(2.0)

            # Drain backlog
            far_client.recv_udp_all(timeout=0.05)

            # leaf sends audio — must NOT reach far
            for _ in range(6):
                leaf_client.send_udp_audio(b"\xDE\xAD" * 80)
                time.sleep(0.02)
            leaf_client.send_udp_flush()
            time.sleep(1.5)

            veto_msgs  = far_client.recv_udp_all(timeout=1.0)
            veto_audio = sum(1 for t, _ in veto_msgs if t == UDP_AUDIO)
            veto_flush = sum(1 for t, _ in veto_msgs if t == UDP_FLUSH)
            self.assertEqual(
                veto_audio + veto_flush, 0,
                f"blacklist veto FAILED: far client received {veto_audio} audio "
                f"+ {veto_flush} flush frames for TG {tg}, expected 0. "
                f"BLACKLIST_TGS=26307 on leaf→natlit should suppress this TG.",
            )

        finally:
            leaf_client.close()
            far_client.close()


if __name__ == "__main__":
    unittest.main(verbosity=2)
