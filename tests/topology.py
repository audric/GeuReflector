"""Single source of truth for the test mesh topology.

Edit REFLECTORS and TEST_PEER below, then run `python3 generate_configs.py`
to regenerate configs/ and docker-compose.test.yml.  The test harness
(test_trunk.py) imports this module at runtime so everything stays in sync.
"""

# ---------------------------------------------------------------------------
# Mesh topology — edit these to change prefixes / add reflectors
# ---------------------------------------------------------------------------

REFLECTORS = {
    #  name       prefixes (list)          host-port-base
    "a": {"prefix": ["122"],  "trunk_port_base": 15000},
    "b": {"prefix": ["121"],  "trunk_port_base": 25000},
    "c": {"prefix": ["1"],    "trunk_port_base": 35000},
}

# Fake trunk peers used by the test harness.
# TEST_PEER:    primary sender (connects as TRUNK_TEST)
# TEST_PEER_RX: passive receiver for audio routing verification (TRUNK_TEST_RX)
TEST_PEER = {
    "prefix": ["9"],
    "secret": "test_secret",
}

TEST_PEER_RX = {
    "prefix": ["8"],
    "secret": "test_secret_rx",
}

# Cluster TGs — forwarded to all peers regardless of prefix ownership
# 8000: no prefix match (not owned by any reflector)
# 1201: starts with prefix "120" (overlaps with reflector-a's ownership)
# 9999: starts with test peer prefix "9"
CLUSTER_TGS = [222, 999]

# Satellite test config (parent is the first reflector in REFLECTORS)
SATELLITE = {
    "id": "SAT_TEST",
    "secret": "sat_secret",
    "listen_port": 5303,
}

# V2 test client credentials (added to [USERS]/[PASSWORDS] in every config)
TEST_CLIENTS = [
    {"callsign": "N0TEST", "group": "TestGroup", "password": "testpass"},
    {"callsign": "N0SEND", "group": "TestGroup", "password": "testpass"},
]

# Shared secret between each pair: sorted tuple of names → secret
# Auto-generated from the pair of reflector names for simplicity
def trunk_secret(name_a: str, name_b: str) -> str:
    pair = tuple(sorted([name_a, name_b]))
    return f"secret_{pair[0]}{pair[1]}"

# ---------------------------------------------------------------------------
# Helpers for prefix handling
# ---------------------------------------------------------------------------

def prefix_str(prefixes) -> str:
    """Join a prefix list into the comma-separated config format."""
    if isinstance(prefixes, str):
        return prefixes
    return ",".join(prefixes)

def prefix_list(prefixes) -> list:
    """Normalize to a list."""
    if isinstance(prefixes, str):
        return [p.strip() for p in prefixes.split(",") if p.strip()]
    return list(prefixes)

def first_prefix(prefixes) -> str:
    """Return the first prefix (used for generating test TG numbers)."""
    return prefix_list(prefixes)[0]

# ---------------------------------------------------------------------------
# Derived constants used by test_trunk.py and generate_configs.py
# ---------------------------------------------------------------------------

# Internal ports inside Docker (fixed)
INTERNAL_CLIENT_PORT = 5300
INTERNAL_TRUNK_PORT = 5302
INTERNAL_HTTP_PORT = 8080

def mapped_trunk_port(name: str) -> int:
    return REFLECTORS[name]["trunk_port_base"] + 302

def mapped_http_port(name: str) -> int:
    return REFLECTORS[name]["trunk_port_base"] + 3080

def mapped_client_port(name: str) -> int:
    return REFLECTORS[name]["trunk_port_base"] + 300

def service_name(name: str) -> str:
    return f"reflector-{name}"

def mapped_satellite_port(name: str) -> int:
    return REFLECTORS[name]["trunk_port_base"] + 303

def trunk_section_name(name_a: str, name_b: str = "") -> str:
    """Shared trunk section name for a link between two reflectors.

    Both sides must use the same section name. Convention: sorted pair.
    If only one name given (legacy), returns TRUNK_<NAME> for test harness use.
    """
    if not name_b:
        return f"TRUNK_{name_a.upper()}"
    pair = tuple(sorted([name_a, name_b]))
    return f"TRUNK_{pair[0].upper()}_{pair[1].upper()}"
