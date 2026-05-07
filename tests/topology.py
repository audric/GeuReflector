"""Single source of truth for the test mesh topology.

Edit REFLECTORS and TEST_PEER below, then run `python3 generate_configs.py`
to regenerate configs/ and docker-compose.test.yml.  The test harness
(test_trunk.py) imports this module at runtime so everything stays in sync.
"""

# ---------------------------------------------------------------------------
# Mesh topology — edit these to change prefixes / add reflectors
# ---------------------------------------------------------------------------

REFLECTORS = {
    #  name       prefixes (list)          host-port-base       redis
    "a": {"prefix": ["122"],  "trunk_port_base": 15000},
    "b": {"prefix": ["121"],  "trunk_port_base": 25000},
    "c": {"prefix": ["1"],    "trunk_port_base": 35000},
    # `d` is sparse-trunked: only connects to `b`. With prefix `1219`
    # (sub-prefix of b's `121`), it lets test_37 verify gateway forwarding
    # — a's audio for TG `121950` must reach d via the b gateway, even
    # though a has no direct trunk to d.
    "d": {"prefix": ["1219"], "trunk_port_base": 45000, "peers": ["b"]},
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

# Third test peer used to exercise per-link BLACKLIST_TGS / ALLOW_TGS /
# TG_MAP / PEER_ID. Kept isolated from TEST_PEER so routing tests don't
# interact with filter tests.
TEST_PEER_FILTER = {
    "prefix":        ["7"],
    "secret":        "test_secret_filter",
    "peer_id":       "filter-peer",
    "blacklist_tgs": "12345",
    "allow_tgs":     "7*,1220",
    "tg_map":        "7000:1220",
}

# Per-reflector MQTT_NAME overrides — when set, the reflector publishes
# under <TOPIC_PREFIX>/<MQTT_NAME>/... so tests can verify the new key
# without breaking the flat-topic MQTT test on reflector 'a'.
MQTT_NAMES = {
    "c": "mqname-c",
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

# A real satellite-mode reflector — runs as a satellite of the primary
# reflector. Used to verify that satellite mode preserves features like
# MQTT publishing and the V2 client server.
SATELLITE_NODE = {
    "name":      "sat",
    "id":        "SAT_NODE",
    "port_base": 55000,
}

# Redis broker — used by reflectors flagged `redis: True`.
# Each reflector uses its own DB index to avoid pollution between tests.
REDIS = {
    "host": "redis",
    "port": 6379,
}

def redis_db_index(name: str) -> int:
    """Stable per-reflector Redis DB index derived from name order."""
    return sorted(REFLECTORS).index(name)

# MQTT test config — broker runs as a Docker service in test compose
MQTT = {
    "host": "mosquitto",
    "port": 1883,
    "username": "test",
    "password": "testpass",
}

# V2 test client credentials (added to [USERS]/[PASSWORDS] in every config)
TEST_CLIENTS = [
    {"callsign": "N0TEST", "group": "TestGroup", "password": "testpass"},
    {"callsign": "N0SEND", "group": "TestGroup", "password": "testpass"},
    {"callsign": "N0THRD", "group": "TestGroup", "password": "testpass"},
]

# Shared secret between each pair: sorted tuple of names → secret
# Auto-generated from the pair of reflector names for simplicity
def trunk_secret(name_a: str, name_b: str) -> str:
    pair = tuple(sorted([name_a, name_b]))
    return f"secret_{pair[0]}{pair[1]}"

def _allowed_peers(name: str) -> set:
    """Other reflector names that `name` is willing to trunk to.

    Default is full mesh (all other reflectors). A reflector can opt into
    a sparse layout by setting `peers: ["x", "y"]` in REFLECTORS — useful
    for testing gateway-style prefix routing.
    """
    me = REFLECTORS[name]
    if "peers" in me:
        return set(me["peers"])
    return {n for n in REFLECTORS if n != name}

def should_trunk(a: str, b: str) -> bool:
    """A trunk between a and b exists iff both sides allow it."""
    return b in _allowed_peers(a) and a in _allowed_peers(b)

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

# ---------------------------------------------------------------------------
# Twin topology — used by E3 integration tests.
# Add TOPOLOGY=twin env var or --topology twin CLI flag to generate_configs.py
# to select this topology without touching the default above.
# ---------------------------------------------------------------------------

# 4-reflector mesh: refa (Italy), ref1+ref2 (Germany twin pair), refc (extra)
#
# Keys:
#   prefix          : list of owned prefixes
#   trunk_port_base : base for host-mapped port offsets
#   twin_of         : name of the twin partner (only present on twin pair members)
#   redis           : enable Redis config store (optional)
TWIN_REFLECTORS = {
    #  name       prefixes     port-base   notes
    # Port layout: client=base+300, trunk=base+302, twin=base+304, http=base+3080
    # Must keep http port <= 65535, so base <= ~62000.
    # Names must be lowercase (Docker image tag constraint).
    "refa": {"prefix": ["222"], "trunk_port_base": 45000},               # Italy
    "ref1": {"prefix": ["262"], "trunk_port_base": 46000, "twin_of": "ref2"},  # DE twin 1
    "ref2": {"prefix": ["262"], "trunk_port_base": 47000, "twin_of": "ref1"},  # DE twin 2
    "refc": {"prefix": ["333"], "trunk_port_base": 48000},               # extra peer
}

# Explicit trunk list for the twin topology.
# Each entry describes one logical link:
#   name          : used as section name base (TRUNK_<NAME>)
#   peers         : list of reflector names involved
#   paired        : if True, the non-pair side emits PAIRED=1 + multi-host HOST=
#   pair_members  : the subset of peers that form the twin pair (needed to split sides)
#
# For a paired trunk the rule is:
#   - non-pair side (refa): [TRUNK_IT_DE] HOST=ref1,ref2 PAIRED=1
#   - pair members (ref1, ref2): [TRUNK_IT_DE] HOST=refa (normal, no PAIRED)
TWIN_TRUNKS = [
    {
        "name":         "IT_DE",
        "peers":        ["refa", "ref1", "ref2"],
        "paired":       True,
        "pair_members": ["ref1", "ref2"],
    },
    {
        "name":         "IT_C",
        "peers":        ["refa", "refc"],
        "paired":       False,
        "pair_members": [],
    },
    {
        "name":         "DE_C",
        "peers":        ["ref1", "ref2", "refc"],
        "paired":       True,
        "pair_members": ["ref1", "ref2"],
    },
]

# Cluster TGs for the twin topology
TWIN_CLUSTER_TGS = [222000, 999]

# Which twin-topology reflector accepts satellite connections. Picking one of
# the twin pair lets tests verify that audio mirrored via the [TWIN] link is
# also forwarded out to satellites attached to the partner.
TWIN_SATELLITE_PARENT = "ref1"

# Shared secret for a trunk link (twin topology uses link name)
def twin_trunk_secret(link_name: str) -> str:
    return f"secret_{link_name.lower()}"

# ---------------------------------------------------------------------------
# Internal ports inside Docker (fixed)
# ---------------------------------------------------------------------------
INTERNAL_CLIENT_PORT = 5300
INTERNAL_TRUNK_PORT = 5302
INTERNAL_HTTP_PORT = 8080
INTERNAL_TWIN_PORT = 5304

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

def sat_node_service_name() -> str:
    return f"reflector-{SATELLITE_NODE['name']}"

def sat_node_parent() -> str:
    """Parent reflector that the satellite-mode node attaches to."""
    return sorted(REFLECTORS)[0]

def sat_node_mapped_client_port() -> int:
    return SATELLITE_NODE["port_base"] + 300

def sat_node_mapped_http_port() -> int:
    return SATELLITE_NODE["port_base"] + 3080

def trunk_section_name(name_a: str, name_b: str = "") -> str:
    """Shared trunk section name for a link between two reflectors.

    Both sides must use the same section name. Convention: sorted pair.
    If only one name given (legacy), returns TRUNK_<NAME> for test harness use.
    """
    if not name_b:
        return f"TRUNK_{name_a.upper()}"
    pair = tuple(sorted([name_a, name_b]))
    return f"TRUNK_{pair[0].upper()}_{pair[1].upper()}"

# ---------------------------------------------------------------------------
# Twin topology helpers
# ---------------------------------------------------------------------------

def twin_mapped_trunk_port(name: str) -> int:
    return TWIN_REFLECTORS[name]["trunk_port_base"] + 302

def twin_mapped_http_port(name: str) -> int:
    return TWIN_REFLECTORS[name]["trunk_port_base"] + 3080

def twin_mapped_client_port(name: str) -> int:
    return TWIN_REFLECTORS[name]["trunk_port_base"] + 300

def twin_mapped_twin_port(name: str) -> int:
    """Host-mapped port for the [TWIN] listen port."""
    return TWIN_REFLECTORS[name]["trunk_port_base"] + 304

def twin_mapped_satellite_port(name: str) -> int:
    """Host-mapped port for the [SATELLITE] listen port in twin topology."""
    return TWIN_REFLECTORS[name]["trunk_port_base"] + 303

def twin_trunks_for(name: str) -> list:
    """Return all TWIN_TRUNKS entries that involve this reflector."""
    return [t for t in TWIN_TRUNKS if name in t["peers"]]
