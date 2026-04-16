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
from test_trunk import ClientPeer, TrunkPeer

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


class RedisOutageTest(unittest.TestCase):
    """Verify reflector tolerates Redis going down and resyncs on resume."""

    def setUp(self):
        flushdb()
        # Populate a baseline user.
        redis_cli("HSET", k("user:N0TEST"), "group", "TestGroup", "enabled", "1")
        redis_cli("HSET", k("group:TestGroup"), "password", "testpass")
        publish("users")
        time.sleep(0.3)

    def _compose(self, *args, check=True):
        cmd = ["docker", "compose", "-f", "docker-compose.redis.yml"] + list(args)
        return subprocess.run(cmd, capture_output=True, text=True, check=check,
                              cwd=os.path.dirname(os.path.abspath(__file__)))

    def test_reflector_survives_outage_and_resyncs(self):
        # Step 1: V2 client connected and authenticated.
        peer = ClientPeer()
        peer.connect(HOST, R1_CLIENT_PORT)
        peer.authenticate(callsign="N0TEST", password="testpass")

        try:
            # Step 2: stop redis container, wait a couple seconds.
            self._compose("stop", "redis")
            time.sleep(2.0)

            # Reflector must still be up (healthcheck is HTTP on port 8080).
            # Connection to existing TCP client should NOT be dropped by the
            # reflector just because Redis went away.
            peer.tcp.settimeout(1.0)
            # Send nothing — just verify the socket isn't RST'd.
            try:
                peer.tcp.send(b"")
            except Exception as e:
                self.fail(f"existing client connection dropped during outage: {e}")

            # Step 3: start redis back up.
            self._compose("start", "redis")

            # Wait for Redis container to become healthy, then for the
            # reflector's exponential backoff to reconnect. Worst case the
            # reflector's backoff was at the 30s cap; typical is 1-4s. Give
            # 60s to accommodate both the container start and a few backoff
            # cycles.
            t_restart = time.time()
            # Wait for redis-cli to succeed first.
            deadline = time.time() + 30.0
            while time.time() < deadline:
                res = self._compose("exec", "-T", "redis",
                                    "redis-cli", "ping", check=False)
                if res.returncode == 0 and "PONG" in res.stdout:
                    break
                time.sleep(0.5)
            else:
                self.fail("redis container did not come back up in 30s")

            # Wait for the reflector to log the full-reload that signals
            # reconnect.
            deadline = time.time() + 60.0
            found = False
            last_logs = ""
            while time.time() < deadline:
                last_logs = docker_logs_since(R1_SVC, t_restart)
                if "config.changed: all" in last_logs:
                    found = True
                    break
                time.sleep(1.0)
            if not found:
                self.fail(
                    "Reflector did not log 'config.changed: all' after "
                    f"Redis resumed. Logs since restart:\n{last_logs[-4000:]}")

            # Step 4: verify post-recovery writes still take effect.
            # N0OTHR is a valid ham-callsign-format string that satisfies the
            # reflector's default ACCEPT_CALLSIGN regex (N + 0 + OTH + R).
            redis_cli("HSET", k("user:N0OTHR"), "group", "OtherGroup",
                      "enabled", "1")
            redis_cli("HSET", k("group:OtherGroup"), "password", "otherpw")
            publish("users")
            time.sleep(0.5)
            self.assertTrue(try_authenticate("N0OTHR", "otherpw"),
                            "post-outage auth for a new user should succeed")

        finally:
            try:
                peer.tcp.close()
            except Exception:
                pass


class RedisImportIdempotenceTest(unittest.TestCase):
    """Verify --import-conf-to-redis is idempotent: running twice against
    an unchanged .conf produces identical Redis state."""

    TEST_DB = 5  # isolated from reflector's DB=0 and other tests
    TEST_PREFIX = "import-test"

    def _redis_cli_db(self, *args) -> str:
        cmd = [
            "docker", "compose", "-f", "docker-compose.redis.yml",
            "exec", "-T", "redis",
            "redis-cli", "-n", str(self.TEST_DB),
        ] + list(args)
        res = subprocess.run(cmd, capture_output=True, text=True, check=True,
                             cwd=os.path.dirname(os.path.abspath(__file__)))
        return res.stdout.strip()

    def _dump_keyspace(self) -> dict:
        """Dump every key's type + content into a dict for comparison."""
        keys = self._redis_cli_db("KEYS", "*").splitlines()
        keys = sorted(k for k in keys if k)
        dump = {}
        for key in keys:
            t = self._redis_cli_db("TYPE", key)
            if t == "hash":
                raw = self._redis_cli_db("HGETALL", key).splitlines()
                dump[key] = ("hash", dict(zip(raw[0::2], raw[1::2])))
            elif t == "set":
                members = sorted(self._redis_cli_db("SMEMBERS", key).splitlines())
                dump[key] = ("set", members)
            elif t == "string":
                dump[key] = ("string", self._redis_cli_db("GET", key))
            else:
                dump[key] = (t, None)
        return dump

    def setUp(self):
        self._redis_cli_db("FLUSHDB")
        # Write the test .conf inside the reflector container using a
        # bash heredoc via docker compose exec.
        conf = f"""[GLOBAL]
LISTEN_PORT=5300
CLUSTER_TGS=222,91,2221

[REDIS]
HOST=redis
PORT=6379
DB={self.TEST_DB}
KEY_PREFIX={self.TEST_PREFIX}

[USERS]
SM0ABC=MyNodes
SM0ABC-1=MyNodes
SM3XYZ=SM3XYZ

[PASSWORDS]
MyNodes=change_me
SM3XYZ=strong_password

[TRUNK_AB]
HOST=peer.example.com
PORT=5302
SECRET=shared_secret
REMOTE_PREFIX=2
BLACKLIST_TGS=666,777*
ALLOW_TGS=24*,2624123
TG_MAP=1:2624123,2:2624124
"""
        # Write via a heredoc in the container.
        subprocess.run([
            "docker", "compose", "-f", "docker-compose.redis.yml",
            "exec", "-T", R1_SVC,
            "bash", "-c",
            "cat > /tmp/import-test.conf"
        ], input=conf, text=True, check=True,
           cwd=os.path.dirname(os.path.abspath(__file__)))

    def _run_importer(self):
        """Invoke --import-conf-to-redis against the test conf."""
        # Path to the binary — discover it dynamically in case the base
        # image places it elsewhere.
        res = subprocess.run([
            "docker", "compose", "-f", "docker-compose.redis.yml",
            "exec", "-T", R1_SVC,
            "bash", "-c",
            "command -v svxreflector"
        ], capture_output=True, text=True, check=True,
           cwd=os.path.dirname(os.path.abspath(__file__)))
        binary = res.stdout.strip()
        self.assertTrue(binary, "svxreflector binary not found in container")

        subprocess.run([
            "docker", "compose", "-f", "docker-compose.redis.yml",
            "exec", "-T", R1_SVC,
            binary, "--config", "/tmp/import-test.conf", "--import-conf-to-redis",
        ], capture_output=True, text=True, check=True,
           cwd=os.path.dirname(os.path.abspath(__file__)))

    def test_import_is_idempotent(self):
        self._run_importer()
        snapshot1 = self._dump_keyspace()

        # Expected key shape — sanity check.
        expected_keys_subset = {
            f"{self.TEST_PREFIX}:user:SM0ABC",
            f"{self.TEST_PREFIX}:user:SM0ABC-1",
            f"{self.TEST_PREFIX}:user:SM3XYZ",
            f"{self.TEST_PREFIX}:group:MyNodes",
            f"{self.TEST_PREFIX}:group:SM3XYZ",
            f"{self.TEST_PREFIX}:cluster:tgs",
            f"{self.TEST_PREFIX}:trunk:AB:blacklist",
            f"{self.TEST_PREFIX}:trunk:AB:allow",
            f"{self.TEST_PREFIX}:trunk:AB:tgmap",
        }
        missing = expected_keys_subset - set(snapshot1.keys())
        self.assertFalse(missing, f"first import missing keys: {missing}")

        self._run_importer()
        snapshot2 = self._dump_keyspace()

        self.assertEqual(snapshot1, snapshot2,
                         "second import produced different Redis state")


class RedisPeerNodeTest(unittest.TestCase):
    """Verify peer node lists received over the trunk are mirrored into
    Redis as live:peer_node:<peer_id>:<callsign> hashes, with removed
    callsigns DEL'd on shrink and full cleanup on trunk disconnect."""

    # Matches topology_redis.TRUNK_TEST (the static trunk section in the
    # generated r1 config).
    SECTION = T.TRUNK_TEST["section"]        # "TRUNK_TEST"
    SECRET  = T.TRUNK_TEST["secret"]         # "test_secret"
    # Reflector's REMOTE_PREFIX for this section — peer must advertise it
    # as its local_prefix to pass the inbound prefix check.
    LOCAL_PREFIX = T.TRUNK_TEST["remote_prefix"]  # "9"

    TRUNK_PORT = T.mapped_trunk_port(R1)

    def setUp(self):
        flushdb()

    def _peer_key(self, callsign: str) -> str:
        return k(f"live:peer_node:{self.SECTION}:{callsign}")

    def _wait_for_hash(self, key: str, present: bool, timeout: float = 3.0):
        deadline = time.time() + timeout
        while time.time() < deadline:
            exists = redis_cli("EXISTS", key).strip() == "1"
            if present and exists:
                return redis_cli("HGETALL", key)
            if (not present) and (not exists):
                return ""
            time.sleep(0.1)
        self.fail(
            f"Timed out waiting for {key} "
            f"{'to exist' if present else 'to be absent'}")

    def _hgetall_dict(self, key: str) -> dict:
        raw = redis_cli("HGETALL", key)
        if not raw:
            return {}
        fields = raw.splitlines()
        return dict(zip(fields[0::2], fields[1::2]))

    def _connect_peer(self) -> TrunkPeer:
        peer = TrunkPeer()
        peer.connect(HOST, self.TRUNK_PORT)
        peer.handshake(trunk_id=self.SECTION,
                       local_prefix=self.LOCAL_PREFIX,
                       secret=self.SECRET)
        return peer

    def test_peer_node_list_creates_and_updates_hashes(self):
        """Inbound node list populates live:peer_node:* hashes;
        subsequent shrunk list DELs dropped callsigns."""
        peer = self._connect_peer()
        try:
            peer.send_node_list([
                ("N1AAA", 123, 0.0, 0.0, ""),
                ("N1BBB", 456, 52.5, 13.4, "Berlin"),
            ])
            self._wait_for_hash(self._peer_key("N1AAA"), present=True)
            self._wait_for_hash(self._peer_key("N1BBB"), present=True)

            aaa = self._hgetall_dict(self._peer_key("N1AAA"))
            self.assertEqual(aaa["callsign"], "N1AAA")
            self.assertEqual(aaa["peer_id"], self.SECTION)
            self.assertEqual(aaa["tg"], "123")
            self.assertIn("updated_at", aaa)
            # No lat/lon/qth expected when lat==lon==0 and qth empty.
            self.assertNotIn("lat", aaa)
            self.assertNotIn("lon", aaa)
            self.assertNotIn("qth_name", aaa)

            bbb = self._hgetall_dict(self._peer_key("N1BBB"))
            self.assertEqual(bbb["tg"], "456")
            self.assertIn("lat", bbb)
            self.assertIn("lon", bbb)
            self.assertEqual(bbb["qth_name"], "Berlin")

            # Shrink the list: drop N1BBB, update N1AAA's TG.
            peer.send_node_list([("N1AAA", 789)])
            self._wait_for_hash(self._peer_key("N1BBB"), present=False)
            deadline = time.time() + 3.0
            while time.time() < deadline:
                aaa = self._hgetall_dict(self._peer_key("N1AAA"))
                if aaa.get("tg") == "789":
                    break
                time.sleep(0.1)
            else:
                self.fail(f"N1AAA tg did not update to 789, got {aaa!r}")
        finally:
            peer.close()

    def test_hostile_strings_are_sanitized(self):
        """Callsigns/QTH with control chars, ':' delimiters, or oversized
        lengths must be stripped/truncated before Redis write. Entries
        whose callsign becomes empty after sanitization must be dropped
        entirely; entries with invalid lat/lon must keep callsign but
        lose the coordinates."""
        peer = self._connect_peer()
        try:
            peer.send_node_list([
                # 1) Control chars + ':' removed from callsign
                ("N1\x01AAA:EVIL", 111, 0.0, 0.0, ""),
                # 2) Over-length callsign is truncated to 32
                ("Z" * 64, 222, 0.0, 0.0, ""),
                # 3) Callsign is only stripped chars → entry dropped
                (":::\x00\x01\x02", 333, 0.0, 0.0, ""),
                # 4) QTH control chars stripped; other bytes kept
                ("N1QTH", 444, 52.5, 13.4, "Bad\x01\x02QTH"),
                # 5) Invalid coordinates → keep callsign, drop lat/lon
                ("N1GEOX", 555, float("nan"), 13.4, ""),
                ("N1GEOY", 556, 52.5, 999.0, ""),
            ])

            # (1) sanitized callsign = "N1AAAEVIL"
            self._wait_for_hash(self._peer_key("N1AAAEVIL"), present=True)
            # (2) truncated to 32 'Z'
            self._wait_for_hash(self._peer_key("Z" * 32), present=True)
            # (3) dropped entirely — no key with the stripped form should exist.
            # Sanity-check by listing live:peer_node:TRUNK_TEST:* and verifying
            # only expected callsigns appear.
            raw = redis_cli("KEYS", k(f"live:peer_node:{self.SECTION}:*"))
            present_keys = set(raw.splitlines()) if raw else set()
            expected = {
                self._peer_key("N1AAAEVIL"),
                self._peer_key("Z" * 32),
                self._peer_key("N1QTH"),
                self._peer_key("N1GEOX"),
                self._peer_key("N1GEOY"),
            }
            self.assertEqual(present_keys, expected,
                f"unexpected peer_node keys: got={sorted(present_keys)} "
                f"expected={sorted(expected)}")

            # (4) QTH stripped of control chars
            qth_entry = self._hgetall_dict(self._peer_key("N1QTH"))
            self.assertEqual(qth_entry["qth_name"], "BadQTH")

            # (5) Invalid coordinates → no lat/lon fields
            geox = self._hgetall_dict(self._peer_key("N1GEOX"))
            self.assertNotIn("lat", geox)
            self.assertNotIn("lon", geox)
            geoy = self._hgetall_dict(self._peer_key("N1GEOY"))
            self.assertNotIn("lat", geoy)
            self.assertNotIn("lon", geoy)
        finally:
            peer.close()

    def test_peer_nodes_cleared_on_trunk_disconnect(self):
        """Closing the trunk link (no other direction active) must DEL all
        of that peer's node keys from Redis."""
        peer = self._connect_peer()
        try:
            peer.send_node_list([("N1AAA", 123), ("N1BBB", 456)])
            self._wait_for_hash(self._peer_key("N1AAA"), present=True)
            self._wait_for_hash(self._peer_key("N1BBB"), present=True)
        finally:
            peer.close()

        # Outbound peer (192.0.2.1) is unroutable, so isActive() becomes
        # false as soon as our inbound closes — cleanup should fire.
        self._wait_for_hash(self._peer_key("N1AAA"), present=False, timeout=5.0)
        self._wait_for_hash(self._peer_key("N1BBB"), present=False, timeout=5.0)


class RedisTrunkAddRemoveTest(unittest.TestCase):
    """Verify dashboard-style add/remove of trunk peers at runtime via
    pub/sub. Uses a dedicated section name so we don't collide with the
    static TRUNK_TEST section from the generated .conf."""

    SECTION = "TRUNK_DYNAMIC_ADD"
    PEER_KEY_SUFFIX = f"trunk:{SECTION}:peer"

    def setUp(self):
        # Redis may still have the key from a previous run — be tidy.
        redis_cli("DEL", k(self.PEER_KEY_SUFFIX))
        # Also publish so any prior dynamic link gets torn down if still
        # around from a previous test run that crashed mid-test.
        publish(f"trunk:{self.SECTION}")
        time.sleep(0.5)

    def _wait_for_log(self, since: float, expected_fragment: str,
                      timeout: float = 3.0) -> str:
        deadline = time.time() + timeout
        last_logs = ""
        while time.time() < deadline:
            last_logs = docker_logs_since(R1_SVC, since)
            for line in last_logs.splitlines():
                if expected_fragment in line:
                    return line
            time.sleep(0.1)
        self.fail(
            f"Timed out waiting for log containing '{expected_fragment}'. "
            f"Last logs:\n{last_logs[-3000:]}")

    def test_add_then_remove_via_pubsub(self):
        # --- add ---
        t0 = time.time()
        redis_cli("HSET", k(self.PEER_KEY_SUFFIX),
                  "host", "192.0.2.77",
                  "port", "5302",
                  "secret", "dyn_secret",
                  "remote_prefix", "8")
        publish(f"trunk:{self.SECTION}")
        line = self._wait_for_log(t0, f"Added trunk link: {self.SECTION}")
        self.assertIn("192.0.2.77", line)

        # --- remove ---
        t1 = time.time()
        redis_cli("DEL", k(self.PEER_KEY_SUFFIX))
        publish(f"trunk:{self.SECTION}")
        self._wait_for_log(t1, f"Removed trunk link: {self.SECTION}")

    def test_add_missing_fields_skipped(self):
        """Peer hash with missing required fields is skipped (no link
        created, no crash)."""
        t0 = time.time()
        # Missing secret and remote_prefix.
        redis_cli("HSET", k(self.PEER_KEY_SUFFIX),
                  "host", "192.0.2.88",
                  "port", "5302")
        publish(f"trunk:{self.SECTION}")
        # Give the reflector time to react.
        time.sleep(1.0)
        logs = docker_logs_since(R1_SVC, t0)
        self.assertNotIn(f"Added trunk link: {self.SECTION}", logs,
                         "Incomplete peer should NOT have been added")
        # And the reflector should not have crashed — check compose ps
        res = subprocess.run([
            "docker", "compose", "-f", "docker-compose.redis.yml",
            "ps", "--status=running", "--services"
        ], capture_output=True, text=True, check=True,
           cwd=os.path.dirname(os.path.abspath(__file__)))
        self.assertIn("reflector-r1", res.stdout)


if __name__ == "__main__":
    unittest.main(verbosity=2)
