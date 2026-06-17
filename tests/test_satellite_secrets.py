"""
Per-satellite secrets — integration tests.

These verify the per-satellite secret resolver rule:

  1. SECRET_<id> match accepts.
  2. SECRET_<id> mismatch rejects (no fallback on mismatch).
  3. Unknown id falls back to [SATELLITE].SECRET.
  4. Malformed SECRET_<bad.id>= keys are ignored at startup; that id
     still authenticates via the default fallback (behavioral
     assertion — see commentary on the test for why we don't grep
     docker logs).
"""

import os
import struct
import time
import unittest

import topology as T

from test_trunk import (
    SAT_SECRET,
    SatellitePeer,
    ROLE_SATELLITE,
    build_trunk_hello,
    get_status,
    _http,
    recv_frame,
    send_frame,
    wait_until,
)


PRIMARY = sorted(T.REFLECTORS)[0]


def _sat_connect(sat_id: str, secret: str) -> SatellitePeer:
    """Connect a satellite peer and run the hello with the given id/secret."""
    peer = SatellitePeer()
    peer.connect_satellite()
    peer.handshake(sat_id=sat_id, secret=secret)
    return peer


def _sat_authenticated_in_status(sat_id: str) -> bool:
    status = get_status(*_http(PRIMARY))
    info = status.get("satellites", {}).get(sat_id, {})
    return bool(info.get("authenticated"))


class SatelliteSecretsTest(unittest.TestCase):

    # 1. Per-id match accepts.
    def test_pinned_secret_accepts(self):
        sat = _sat_connect(T.SATELLITE_PINNED_ID, T.SATELLITE_PINNED_SECRET)
        try:
            wait_until(
                lambda: _sat_authenticated_in_status(T.SATELLITE_PINNED_ID),
                timeout=5.0,
                msg="pinned satellite never authenticated",
            )
        finally:
            sat.close()

    # 2. Per-id mismatch rejects (does NOT fall back to default SECRET).
    def test_pinned_id_with_wrong_secret_rejects(self):
        """Pinned id presented with default (wrong-for-this-id) secret is rejected.

        Sends the hello frame raw rather than via SatellitePeer.handshake(),
        because handshake() blocks on recv_msg() for the parent's reply and
        the parent closes the socket on auth failure — meaning the rejection
        raises inside handshake() rather than on a subsequent recv_frame.
        """
        peer = SatellitePeer()
        peer.connect_satellite()
        priority = struct.unpack("!I", os.urandom(4))[0]
        hello = build_trunk_hello(T.SATELLITE_PINNED_ID, "", priority,
                                  SAT_SECRET, role=ROLE_SATELLITE)
        send_frame(peer.sock, hello)
        peer.sock.settimeout(5.0)
        with self.assertRaises((ConnectionError, OSError, struct.error)):
            recv_frame(peer.sock)
        peer.close()
        # Confirm /status never marked the pinned id authenticated.
        time.sleep(0.5)
        self.assertFalse(
            _sat_authenticated_in_status(T.SATELLITE_PINNED_ID),
            "pinned satellite must not authenticate with the default secret",
        )

    # 3. Unknown id falls back to default SECRET.
    def test_unknown_id_falls_back_to_default(self):
        sat = _sat_connect(T.SATELLITE_UNPINNED_ID, SAT_SECRET)
        try:
            wait_until(
                lambda: _sat_authenticated_in_status(T.SATELLITE_UNPINNED_ID),
                timeout=5.0,
                msg="unpinned satellite did not authenticate via fallback",
            )
        finally:
            sat.close()

    # 4. Regression: a rejected hello must not leave a SatelliteLink whose
    #    heartbeat timer keeps ticking against a closed socket. Pre-fix,
    #    the parent asserted in TcpConnection::write ~10s after rejection
    #    (because the heartbeat timer was constructed enabled and never
    #    got disabled when the hello was rejected). Send a bad hello,
    #    wait past the 10s tx-heartbeat threshold, then confirm /status
    #    still responds (i.e. the parent process is alive).
    def test_rejection_does_not_crash_parent(self):
        peer = SatellitePeer()
        peer.connect_satellite()
        priority = struct.unpack("!I", os.urandom(4))[0]
        hello = build_trunk_hello(T.SATELLITE_PINNED_ID, "", priority,
                                  SAT_SECRET, role=ROLE_SATELLITE)
        send_frame(peer.sock, hello)
        peer.close()
        # Past the t=10s heartbeat tx tick where the pre-fix crash fired.
        time.sleep(11.0)
        status = get_status(*_http(PRIMARY))
        self.assertIsInstance(status, dict, "parent /status not responding")
        self.assertNotIn(T.SATELLITE_PINNED_ID, status.get("satellites", {}),
                         "phantom satellite entry after rejection")

    # 5. Malformed SECRET_<bad.id>= is ignored at startup; the id still
    #    authenticates via the default fallback.
    #
    # We don't grep docker logs for the startup warning here (rolling
    # log windows flake on warm meshes). Instead we rely on a behavioral
    # assertion: if the malformed SECRET_bad.id=ignored_by_startup_validator
    # entry were processed, the satellite's HMAC (computed over SAT_SECRET)
    # would not verify against that value and auth would fail. Reaching
    # /status with authenticated=true means the malformed key was ignored.
    def test_malformed_secret_key_ignored_and_falls_back(self):
        sat = _sat_connect(T.SATELLITE_BAD_KEY_ID, SAT_SECRET)
        try:
            wait_until(
                lambda: _sat_authenticated_in_status(T.SATELLITE_BAD_KEY_ID),
                timeout=5.0,
                msg="bad-key-id satellite did not authenticate via fallback",
            )
        finally:
            sat.close()


if __name__ == "__main__":
    unittest.main()
