"""Minimal topology for Redis-backed test harness.

Edit here, then run `python3 generate_redis_configs.py` to regenerate
configs-redis/ and docker-compose.redis.yml.
"""

# Single Redis-enabled reflector for the Redis test suite.
REFLECTORS = {
    "r1": {"prefix": ["1"], "trunk_port_base": 45000, "redis": True},
}

REDIS = {
    "host": "redis",
    "port": 6379,
}

# V2 test client credentials (populated into Redis by tests themselves).
TEST_CLIENTS = [
    {"callsign": "N0TEST",  "group": "TestGroup", "password": "testpass"},
    {"callsign": "N0OTHER", "group": "OtherGroup", "password": "otherpass"},
]

# Placeholder trunk peer — no real peer is running, but its [TRUNK_TEST]
# section makes the reflector instantiate a TrunkLink whose reloadConfig()
# we can trigger via pub/sub and verify via container logs.
TRUNK_TEST = {
    "section":        "TRUNK_TEST",
    "host":           "192.0.2.1",   # TEST-NET-1 — guaranteed unroutable
    "port":           59302,
    "secret":         "test_secret",
    "remote_prefix":  "9",
}

# Internal container ports (same as legacy harness so the reflector
# binary's defaults don't change).
INTERNAL_CLIENT_PORT = 5300
INTERNAL_TRUNK_PORT  = 5302
INTERNAL_HTTP_PORT   = 8080

def service_name(name: str) -> str:
    return f"reflector-{name}"

def mapped_client_port(name: str) -> int:
    return REFLECTORS[name]["trunk_port_base"] + 300

def mapped_http_port(name: str) -> int:
    return REFLECTORS[name]["trunk_port_base"] + 3080

def redis_port() -> int:
    return 46379

def redis_db_index(name: str) -> int:
    return sorted(REFLECTORS).index(name)
