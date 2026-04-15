#!/usr/bin/env python3
"""Integration tests for Redis-backed config store.

Uses the separate Redis test harness (topology_redis.py +
docker-compose.redis.yml). Reuses ClientPeer from test_trunk.py for V2
client authentication.
"""

import os
import subprocess
import sys
import time
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import topology_redis as T
from test_trunk import ClientPeer

HOST = "127.0.0.1"

R1 = "r1"
R1_SVC = T.service_name(R1)
R1_CLIENT_PORT = T.mapped_client_port(R1)
R1_DB = T.redis_db_index(R1)
R1_KEY_PREFIX = T.service_name(R1)


def redis_cli(*args) -> str:
    """Run a redis-cli command against the harness's Redis container
    (against the reflector's DB index). Returns stdout (stripped)."""
    cmd = [
        "docker", "compose", "-f", "docker-compose.redis.yml",
        "exec", "-T", "redis",
        "redis-cli", "-n", str(R1_DB),
    ] + list(args)
    res = subprocess.run(cmd, capture_output=True, text=True, check=True,
                         cwd=os.path.dirname(os.path.abspath(__file__)))
    return res.stdout.strip()


def publish(scope: str):
    redis_cli("PUBLISH", f"{R1_KEY_PREFIX}:config.changed", scope)


def k(suffix: str) -> str:
    """Prepend the reflector's key prefix."""
    return f"{R1_KEY_PREFIX}:{suffix}"


def flushdb():
    redis_cli("FLUSHDB")


def try_authenticate(callsign: str, password: str,
                     port: int = R1_CLIENT_PORT,
                     timeout: float = 3.0) -> bool:
    """Attempt V2 client auth against a reflector. Returns True on success,
    False on rejection/timeout."""
    peer = ClientPeer()
    try:
        peer.connect(HOST, port, timeout=timeout)
        peer.authenticate(callsign=callsign, password=password)
        return True
    except Exception:
        return False
    finally:
        if peer.tcp is not None:
            try:
                peer.tcp.close()
            except Exception:
                pass


def docker_logs_since(service: str, since: float) -> str:
    """Return stdout/stderr from the given compose service since `since`
    (Unix timestamp). Used to assert on reflector log lines produced
    after a specific test action."""
    from datetime import datetime, timezone
    since_iso = datetime.fromtimestamp(since, tz=timezone.utc).isoformat()
    cmd = [
        "docker", "compose", "-f", "docker-compose.redis.yml",
        "logs", "--since", since_iso, service,
    ]
    res = subprocess.run(cmd, capture_output=True, text=True, check=True,
                         cwd=os.path.dirname(os.path.abspath(__file__)))
    return res.stdout + res.stderr


class RedisUsersTest(unittest.TestCase):
    def setUp(self):
        flushdb()

    def test_user_add_then_authenticate(self):
        """HSET a user + group, publish, and verify the reflector accepts
        authentication within 1 s."""
        redis_cli("HSET", k("user:N0TEST"), "group", "TestGroup", "enabled", "1")
        redis_cli("HSET", k("group:TestGroup"), "password", "testpass")
        publish("users")
        # No wait needed in theory — new connections re-query Redis every
        # login — but allow a small settle to account for container clock
        # skew and broker latency.
        time.sleep(0.3)
        self.assertTrue(try_authenticate("N0TEST", "testpass"),
                        "expected N0TEST to authenticate after Redis write")

    def test_user_disable_rejects_auth(self):
        """Disabling a user via enabled=0 must cause auth to fail."""
        redis_cli("HSET", k("user:N0TEST"), "group", "TestGroup", "enabled", "1")
        redis_cli("HSET", k("group:TestGroup"), "password", "testpass")
        publish("users")
        time.sleep(0.3)
        self.assertTrue(try_authenticate("N0TEST", "testpass"))

        redis_cli("HSET", k("user:N0TEST"), "enabled", "0")
        publish("users")
        time.sleep(0.3)
        self.assertFalse(try_authenticate("N0TEST", "testpass"),
                         "expected N0TEST to be rejected after enabled=0")


class RedisTrunkFilterTest(unittest.TestCase):
    SECTION = "TRUNK_TEST"

    def setUp(self):
        flushdb()

    def _wait_for_reload_log(self, since: float, expected_fragment: str,
                             timeout: float = 3.0) -> str:
        """Poll container logs until a 'Reloaded filters' line appears
        that contains `expected_fragment`. Returns the matching line."""
        deadline = time.time() + timeout
        last_logs = ""
        while time.time() < deadline:
            last_logs = docker_logs_since(R1_SVC, since)
            for line in last_logs.splitlines():
                if "Reloaded filters" in line and expected_fragment in line:
                    return line
            time.sleep(0.1)
        self.fail(
            f"Timed out waiting for 'Reloaded filters' with "
            f"'{expected_fragment}'. Last logs:\n{last_logs}")

    def test_blacklist_change_triggers_reload(self):
        # Baseline: no blacklist key → nothing in Redis for this section.
        # Add one and verify the reload log shows it.
        t0 = time.time()
        redis_cli("SADD", k(f"trunk:{self.SECTION}:blacklist"), "666")
        publish(f"trunk:{self.SECTION}")
        line = self._wait_for_reload_log(t0, "blacklist=666")
        self.assertIn("TRUNK_TEST", line)

        # Remove it, verify blacklist goes away.
        t1 = time.time()
        redis_cli("SREM", k(f"trunk:{self.SECTION}:blacklist"), "666")
        publish(f"trunk:{self.SECTION}")
        # After SREM the set is empty — the reload log will lack
        # "blacklist=" entirely. Poll for a reload line that mentions
        # the section but has no blacklist tag.
        deadline = time.time() + 3.0
        while time.time() < deadline:
            logs = docker_logs_since(R1_SVC, t1)
            for ln in logs.splitlines():
                if ("TRUNK_TEST" in ln and "Reloaded filters" in ln
                        and "blacklist=" not in ln):
                    return
            time.sleep(0.1)
        self.fail(f"Timed out waiting for reload without blacklist. Logs:\n{logs}")

    def test_allow_change_triggers_reload(self):
        t0 = time.time()
        redis_cli("SADD", k(f"trunk:{self.SECTION}:allow"), "24*")
        publish(f"trunk:{self.SECTION}")
        line = self._wait_for_reload_log(t0, "allow=24*")
        self.assertIn("TRUNK_TEST", line)


class RedisPtyMuteTest(unittest.TestCase):
    SECTION = "TRUNK_TEST"

    def setUp(self):
        flushdb()

    def _pty_send(self, command: str) -> None:
        """Write a command line to the reflector's PTY inside the
        container. The existing COMMAND_PTY config lives at
        /dev/shm/reflector_ctrl."""
        subprocess.run([
            "docker", "compose", "-f", "docker-compose.redis.yml",
            "exec", "-T", R1_SVC,
            "bash", "-c",
            f"printf '{command}\\n' > /dev/shm/reflector_ctrl"
        ], check=True,
           cwd=os.path.dirname(os.path.abspath(__file__)))

    def _wait_for_members(self, key: str, expected_cs: str, present: bool,
                          timeout: float = 3.0) -> None:
        deadline = time.time() + timeout
        last = ""
        while time.time() < deadline:
            last = redis_cli("SMEMBERS", key)
            members = set(filter(None, last.splitlines()))
            if present and expected_cs in members:
                return
            if (not present) and expected_cs not in members:
                return
            time.sleep(0.1)
        self.fail(
            f"Timed out waiting for {expected_cs} "
            f"{'in' if present else 'absent from'} {key}. "
            f"Last SMEMBERS: {last!r}")

    def test_pty_mute_then_unmute_persists_to_redis(self):
        key = k(f"trunk:{self.SECTION}:mutes")
        # Baseline: empty
        self.assertEqual(redis_cli("SMEMBERS", key), "")

        self._pty_send(f"TRUNK MUTE {self.SECTION} SM0XYZ")
        self._wait_for_members(key, "SM0XYZ", present=True)

        self._pty_send(f"TRUNK UNMUTE {self.SECTION} SM0XYZ")
        self._wait_for_members(key, "SM0XYZ", present=False)


class RedisLiveStateTest(unittest.TestCase):
    """Verify live:client:<callsign> hashes appear/disappear as V2 clients
    connect and disconnect. Talker and trunk live-state are not covered here
    (require UDP/trunk-peer scaffolding; deferred)."""

    def setUp(self):
        flushdb()
        # Populate the user so authentication succeeds.
        redis_cli("HSET", k("user:N0TEST"), "group", "TestGroup", "enabled", "1")
        redis_cli("HSET", k("group:TestGroup"), "password", "testpass")
        publish("users")
        time.sleep(0.3)

    def _wait_for_hash(self, key: str, present: bool,
                       timeout: float = 3.0):
        """Poll HGETALL until the key has/lacks fields. Redis TYPE is
        checked via EXISTS — empty hashes don't exist."""
        deadline = time.time() + timeout
        last = ""
        while time.time() < deadline:
            exists = redis_cli("EXISTS", key).strip() == "1"
            if present and exists:
                last = redis_cli("HGETALL", key)
                return last
            if (not present) and (not exists):
                return ""
            time.sleep(0.1)
        self.fail(
            f"Timed out waiting for {key} "
            f"{'to exist' if present else 'to be absent'}. Last: {last!r}")

    def test_live_client_hash_appears_on_auth(self):
        peer = ClientPeer()
        peer.connect(HOST, R1_CLIENT_PORT)
        try:
            peer.authenticate(callsign="N0TEST", password="testpass")
            raw = self._wait_for_hash(k("live:client:N0TEST"), present=True)
            # HGETALL returns alternating field\nvalue\nfield\nvalue\n
            fields = raw.splitlines()
            mapping = dict(zip(fields[0::2], fields[1::2]))
            self.assertIn("connected_at", mapping)
            self.assertIn("ip", mapping)
            self.assertIn("tg", mapping)
            # codecs may be "" — just verify the field exists
            self.assertIn("codecs", mapping)
            # connected_at is a unix timestamp; sanity-check it's a
            # reasonable value (positive, within last minute).
            ts = int(mapping["connected_at"])
            self.assertGreater(ts, 0)
            self.assertLess(abs(ts - time.time()), 60)
        finally:
            try:
                peer.tcp.close()
            except Exception:
                pass

    def test_live_client_hash_disappears_on_disconnect(self):
        peer = ClientPeer()
        peer.connect(HOST, R1_CLIENT_PORT)
        peer.authenticate(callsign="N0TEST", password="testpass")
        self._wait_for_hash(k("live:client:N0TEST"), present=True)

        # Forceful close — reflector should see disconnect quickly.
        peer.tcp.close()

        # Drain queue + Redis publish should take <100ms. Give more for slack.
        self._wait_for_hash(k("live:client:N0TEST"), present=False, timeout=5.0)


if __name__ == "__main__":
    unittest.main(verbosity=2)
