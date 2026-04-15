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


if __name__ == "__main__":
    unittest.main(verbosity=2)
