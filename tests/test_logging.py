#!/usr/bin/env python3
"""Integration tests for GeuReflector logging facade (geulog::).

Exercises the LOG PTY commands (SHOW, SET, RESET), level filtering, and
error rejection via the /dev/shm/reflector_ctrl PTY inside a running
reflector container.

Requires: Python 3.7+, stdlib only.
Docker must be running with the default topology:
    cd tests && python3 generate_configs.py
    docker compose -f docker-compose.test.yml up -d --build --wait
"""

import os
import subprocess
import sys
import time
import unittest

# Ensure tests/ is on the path so topology can be imported
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import topology as T

TESTS_DIR = os.path.dirname(os.path.abspath(__file__))
COMPOSE_FILE = "docker-compose.test.yml"

# All seven subsystems defined by geulog:: (must stay in sync with Log.h)
EXPECTED_SUBSYSTEMS = ["trunk", "satellite", "twin", "client", "mqtt", "redis", "core"]

# Primary reflector used for all logging tests (sorted first = "a")
PRIMARY = sorted(T.REFLECTORS)[0]


# ---------------------------------------------------------------------------
# Helpers (match the idiom in test_trunk.py exactly)
# ---------------------------------------------------------------------------

def pty_send(reflector_name: str, command: str, timeout: float = 5.0) -> str:
    """Write a line to /dev/shm/reflector_ctrl inside a reflector container."""
    svc = T.service_name(reflector_name)
    proc = subprocess.run(
        ["docker", "compose", "-f", COMPOSE_FILE,
         "exec", "-T", svc,
         "bash", "-c", f"printf '%s\\n' {repr(command)} > /dev/shm/reflector_ctrl"],
        cwd=TESTS_DIR,
        capture_output=True, text=True, timeout=timeout,
    )
    if proc.returncode != 0:
        raise RuntimeError(
            f"pty_send to {svc} failed: rc={proc.returncode} "
            f"stderr={proc.stderr.strip()}")
    return proc.stdout


def pty_send_recv(reflector_name: str, command: str,
                  wait: float = 0.5, timeout: float = 8.0) -> str:
    """Write a command to the PTY and capture the response written back.

    Starts a background reader on the PTY slave, sends the command, waits
    for the reflector to process and write its response, then returns what
    was captured.
    """
    svc = T.service_name(reflector_name)
    # Start a background cat reading from the PTY slave (response side),
    # sleep briefly to let it start, then write the command, wait for the
    # reflector to respond, kill the reader and return what was captured.
    script = (
        f"timeout {wait} cat /dev/shm/reflector_ctrl > /tmp/pty_resp 2>&1 & "
        f"BGPID=$!; "
        f"sleep 0.05; "
        f"printf '%s\\n' {repr(command)} > /dev/shm/reflector_ctrl; "
        f"wait $BGPID; "
        f"cat /tmp/pty_resp"
    )
    proc = subprocess.run(
        ["docker", "compose", "-f", COMPOSE_FILE,
         "exec", "-T", svc, "bash", "-c", script],
        cwd=TESTS_DIR,
        capture_output=True, text=True, timeout=timeout,
    )
    return proc.stdout + proc.stderr


def docker_logs_since(reflector_name: str, since: str = "5s") -> str:
    """Return the stdout/stderr of a reflector container since N seconds ago."""
    svc = T.service_name(reflector_name)
    proc = subprocess.run(
        ["docker", "compose", "-f", COMPOSE_FILE,
         "logs", "--no-color", "--since", since, svc],
        cwd=TESTS_DIR,
        capture_output=True, text=True, timeout=10.0,
    )
    return proc.stdout + proc.stderr


# ---------------------------------------------------------------------------
# Test cases
# ---------------------------------------------------------------------------

class TestLoggingFacade(unittest.TestCase):

    @classmethod
    def setUpClass(cls):
        """Ensure the primary reflector is reachable before running tests."""
        from urllib.request import urlopen
        from urllib.error import URLError

        host = "127.0.0.1"
        http_port = T.mapped_http_port(PRIMARY)
        url = f"http://{host}:{http_port}/status"
        deadline = time.time() + 30.0
        while True:
            try:
                with urlopen(url, timeout=2.0) as resp:
                    resp.read()
                break
            except (URLError, OSError):
                if time.time() > deadline:
                    raise RuntimeError(
                        f"reflector-{PRIMARY} not reachable at {url}")
                time.sleep(0.5)

        # Reset log levels to defaults before the suite runs so tests
        # start from a known state regardless of prior runs.
        pty_send(PRIMARY, "LOG RESET")
        time.sleep(0.2)

    def tearDown(self):
        """Restore default log levels after each test."""
        pty_send(PRIMARY, "LOG RESET")
        time.sleep(0.1)

    # ------------------------------------------------------------------
    # Test 01: LOG SHOW returns a snapshot containing all subsystems
    # ------------------------------------------------------------------
    def test_01_log_show_returns_snapshot(self):
        """LOG SHOW returns a <sub>=<lvl> snapshot for every subsystem."""
        out = pty_send_recv(PRIMARY, "LOG SHOW")
        for sub in EXPECTED_SUBSYSTEMS:
            self.assertIn(
                f"{sub}=", out,
                f"subsystem '{sub}' missing from LOG SHOW output: {out!r}")

    # ------------------------------------------------------------------
    # Test 02: Setting trunk=debug produces [debug] [trunk] lines
    # ------------------------------------------------------------------
    def test_02_set_level_to_debug_produces_debug_lines(self):
        """After LOG trunk=debug, heartbeat/peer messages appear at debug level."""
        # Set trunk to debug and record the timestamp
        pty_send(PRIMARY, "LOG trunk=debug")
        time.sleep(0.2)
        t0 = time.time()

        # Wait up to 20 s for a [debug] [trunk] line to appear in docker logs.
        # Trunk heartbeats fire every 10 s so we give it enough headroom.
        deadline = t0 + 20.0
        found = False
        while time.time() < deadline:
            # Request fresh logs relative to when we set the level
            elapsed = time.time() - t0
            since = f"{int(elapsed) + 2}s"
            logs = docker_logs_since(PRIMARY, since=since)
            if "[debug] [trunk]" in logs:
                found = True
                break
            time.sleep(1.0)

        self.assertTrue(
            found,
            "No '[debug] [trunk]' line appeared within 20 s after LOG trunk=debug")

    # ------------------------------------------------------------------
    # Test 03: Setting trunk=error suppresses debug/info lines
    # ------------------------------------------------------------------
    def test_03_set_level_to_error_suppresses_lower_levels(self):
        """After LOG trunk=error, no [debug] or [info] [trunk] lines appear."""
        # Raise the level to error.
        pty_send(PRIMARY, "LOG trunk=error")

        # Wait slightly beyond one full heartbeat cycle (10 s) so that if
        # any debug/info lines were going to appear they would have done so.
        time.sleep(13.0)

        # Only look at the last 12 s of logs — entirely within the
        # error-level window (the level was set >13 s ago so there is at
        # least 12 s of error-level coverage captured here).
        logs = docker_logs_since(PRIMARY, since="12s")

        low_level_lines = [
            line for line in logs.splitlines()
            if "[debug] [trunk]" in line or "[info] [trunk]" in line
        ]
        self.assertEqual(
            low_level_lines, [],
            f"Found unexpected low-level trunk lines after LOG trunk=error: "
            f"{low_level_lines[:5]}")

    # ------------------------------------------------------------------
    # Test 04: LOG RESET restores the startup snapshot
    # ------------------------------------------------------------------
    def test_04_reset_restores_startup_snapshot(self):
        """After LOG <sub>=debug then LOG RESET, snapshot is back to default."""
        # Change a level
        pty_send(PRIMARY, "LOG trunk=debug")
        time.sleep(0.2)

        # Verify the change took effect via SHOW
        after_change = pty_send_recv(PRIMARY, "LOG SHOW")
        self.assertIn("trunk=debug", after_change,
                      f"Level did not change to debug: {after_change!r}")

        # Reset and read back
        pty_send(PRIMARY, "LOG RESET")
        time.sleep(0.2)
        after_reset = pty_send_recv(PRIMARY, "LOG SHOW")

        # The startup config has no LOG= key so everything defaults to info
        self.assertIn(
            "trunk=info", after_reset,
            f"After RESET, expected trunk=info in snapshot: {after_reset!r}")

    # ------------------------------------------------------------------
    # Test 05: Unknown subsystem is rejected with an error message
    # ------------------------------------------------------------------
    def test_05_unknown_subsystem_rejected(self):
        """LOG bogus=debug returns an error containing 'unknown subsystem or level'."""
        out = pty_send_recv(PRIMARY, "LOG bogus=debug")
        self.assertIn(
            "unknown subsystem or level", out,
            f"Expected error message not found in PTY response: {out!r}")

    # ------------------------------------------------------------------
    # Test 06: Unknown level is rejected with an error message
    # ------------------------------------------------------------------
    def test_06_unknown_level_rejected(self):
        """LOG trunk=yelling returns an error containing 'unknown subsystem or level'."""
        out = pty_send_recv(PRIMARY, "LOG trunk=yelling")
        self.assertIn(
            "unknown subsystem or level", out,
            f"Expected error message not found in PTY response: {out!r}")


if __name__ == "__main__":
    unittest.main(verbosity=2)
