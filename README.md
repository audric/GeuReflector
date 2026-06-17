# GeuReflector

GeuReflector is a fork of [SvxReflector](https://github.com/sm0svx/svxlink)
extended with a **server-to-server trunk protocol** that lets multiple reflector
instances share talk groups (TGs) as independent parallel voice channels —
analogous to telephone trunk lines between telephone exchanges.

Original SvxReflector by Tobias Blomberg / SM0SVX.
Trunk extension by IW1GEU.

---

## Contents

- [New to SvxLink reflectors? Start here](#new-to-svxlink-reflectors-start-here)
- [Quick start — one standalone reflector](#quick-start--one-standalone-reflector)
- [Which setup fits my deployment?](#which-setup-fits-my-deployment)
- [Why trunking?](#why-trunking)
- [What it adds over SvxReflector](#what-it-adds-over-svxreflector)
- [How TG ownership works](#how-tg-ownership-works)
- [Build](#build) · [Testing](#testing) · [Configuration](#configuration)
- [HTTP Status](#http-status) · [Live user/password reload](#live-userpassword-reload)
- [Documentation](#documentation) · [License](#license)

---

## New to SvxLink reflectors? Start here

If you already run SvxReflector, skip to [What it adds](#what-it-adds-over-svxreflector).
Otherwise, here is the whole idea:

- **SvxLink nodes** are radios (repeaters, hotspots, simplex links) connected to
  the internet instead of to each other over RF.
- A **reflector** is the central server those nodes connect to — it relays voice
  between every node that is listening to the same channel, like a conference
  bridge for radios.
- A **talk group (TG)** is one of those channels, identified by a number. A node
  picks a TG and hears everyone else on that TG.
- Normally one reflector serves everyone. **GeuReflector lets you run several
  reflectors and link them together** ("trunking"), so a talk group can span
  multiple servers while each server stays independent.

### Glossary

| Term | Meaning |
|------|---------|
| **Node** | An SvxLink instance (a radio gateway). Connects to a reflector as a client. Unmodified by this fork. |
| **Reflector** | The server that relays audio between nodes on the same talk group. |
| **Talk group (TG)** | A numbered voice channel. One talker at a time per TG. |
| **Trunk** | A persistent server-to-server link between two reflectors that carries shared TGs. |
| **Prefix** | The leading digits of a TG number that decide which reflector *owns* it (e.g. prefix `262` owns TGs `262`, `2620`, `26299`…). |
| **Cluster TG** | A TG broadcast to every trunked reflector regardless of prefix — like a nationwide channel. |
| **Satellite** | A lightweight reflector that hangs off a parent instead of joining the full mesh (NAT-friendly). |
| **Twin** | Two reflectors sharing one prefix as a high-availability pair, appearing as one peer to the mesh. |

---

## Quick start — one standalone reflector

You do **not** need trunking to run GeuReflector. With no `[TRUNK_x]` sections it
behaves exactly like stock SvxReflector, so the fastest way to see it working is
a single server. (Docker is easiest; see [`docs/DOCKER.md`](docs/DOCKER.md) for
the full container guide, or [`docs/INSTALL.md`](docs/INSTALL.md) to build from
source.)

1. **Start from the sample config.** The shipped `svxreflector.conf` already has
   the `[GLOBAL]`, certificate, `[USERS]`, and `[PASSWORDS]` sections you need.
   Copy it and edit two things:

   ```ini
   [GLOBAL]
   LISTEN_PORT=5300
   LOCAL_PREFIX=262          # optional for a single server; sets which TGs it "owns"

   [USERS]
   MYNODE-1=MyNodes          # the callsign your SvxLink node logs in with

   [PASSWORDS]
   MyNodes="change-this-secret"
   ```

   That is the only GeuReflector-specific addition for a standalone server —
   everything else is stock SvxReflector configuration.

2. **Run it** (Docker):

   ```bash
   mkdir config && cp /path/to/svxreflector.conf config/svxreflector.conf
   docker compose up -d
   docker compose logs -f
   ```

3. **Connect a node.** Point an SvxLink node at this server (host + port 5300,
   the callsign/password above), key up on a talk group, and watch the talker
   appear in the logs. Enable `HTTP_SRV_PORT=8080` to see live state at
   `http://<host>:8080/status`.

Once one reflector works, [add trunk links](#4-add-a-trunk-section-per-peer) to
connect it to others.

---

## Which setup fits my deployment?

| Your situation | Setup | Where to look |
|----------------|-------|---------------|
| One reflector for a club or single site | Standalone (no trunks) | [Quick start](#quick-start--one-standalone-reflector) above |
| Several regions in one country | Full trunk mesh | [Full-mesh example](#full-mesh-example--three-reflectors), [`docs/DEPLOYMENT_ITALY.md`](docs/DEPLOYMENT_ITALY.md) |
| Many countries / a backbone | Full mesh + gateways + routable prefixes | [`docs/WW_DEPLOYMENT.md`](docs/WW_DEPLOYMENT.md), [Routable prefixes](#5-routable-prefixes--hierarchical-and-non-full-mesh-topologies-optional) |
| Members behind NAT / no static IP | Satellites under a parent | [Satellite reflectors](#satellite-reflectors) |
| Failover for a single reflector | Twin (HA) pair | [`docs/TWIN_PROTOCOL.md`](docs/TWIN_PROTOCOL.md) |

For diagrams of each shape and a fuller decision guide, see
[`docs/TOPOLOGY_EXAMPLES.md`](docs/TOPOLOGY_EXAMPLES.md). For *why* these pieces
work the way they do, read [`docs/CONCEPTS.md`](docs/CONCEPTS.md).

---

## Why trunking?

A standard SvxReflector is a single centralized server. Without trunking, a
multi-site national deployment has two unsatisfying options:

- **One national server** — only one TG can be active at a time across the
  entire network; one outage takes everyone down.
- **Independent regional servers bridged by SvxLink instances** — technically
  possible, but each bridge instance handles only one TG at a time and one
  instance is needed per pair of servers. For 20 regions in full mesh that is
  190 SvxLink bridge processes for a single shared TG, multiplied by the number
  of TGs to share. Unmanageable at scale.

GeuReflector solves this: each region runs its own independent reflector
(resilience, local autonomy) and the trunk links connect them so they can share
TGs selectively. All regional TGs can carry simultaneous independent QSOs while
a national TG remains available to all. Talker arbitration is handled
automatically.

---

## What it adds over SvxReflector

- **Prefix-based TG ownership** — each reflector is authoritative for all TGs
  whose decimal representation starts with a configured digit string (e.g.
  `LOCAL_PREFIX=1` → owns TGs 1, 10, 100, 1000, 12345 …). Both `LOCAL_PREFIX`
  and `REMOTE_PREFIX` accept comma-separated lists for reflectors that own
  multiple prefix groups (e.g. `LOCAL_PREFIX=11,12,13`)
- **Server-to-server trunk links** — persistent TCP connections between pairs of
  reflectors carry talker state and audio for shared TGs
- **Full-mesh topology with gateway prefix routing** — any number of
  reflectors can be trunked together; every pair has a direct link inside a
  mesh. Audio distribution uses **owner-relay**: a non-owner sends to the
  TG's owner, and the owner fans the audio out to every other trunk peer
  with interest. A reflector with two trunk legs (e.g. a country gateway
  between an international mesh and a national mesh) automatically bridges
  inbound traffic toward the longest-prefix-match owner via
  `Reflector::shouldRelayInbound` — no `CLUSTER_TGS` hack required.
  Cluster TGs keep an anti-loop single-hop semantic. See
  [`docs/TOPOLOGY_EXAMPLES.md`](docs/TOPOLOGY_EXAMPLES.md) for a visual
  cookbook of the five supported shapes (international full-mesh,
  national mesh joined at gateway, satellite tree, hybrid
  satellite-under-regional, HA twin pair)
- **Unlimited concurrent conversations** — the trunk TCP connection multiplexes
  all active TGs simultaneously; the only per-TG rule is one talker at a time
- **Independent talker arbitration** — each reflector arbitrates its own clients;
  the trunk adds a tie-breaking layer (random priority nonce) for simultaneous
  claims
- **Cluster TGs** — BrandMeister-style nationwide talk groups that are broadcast
  to all trunk peers regardless of prefix ownership
- **Twin (HA-pair) protocol** — two reflectors can share `LOCAL_PREFIX` and
  appear as one logical peer to external trunks, with sticky per-transmission
  socket selection and instant failover on socket failure. See
  `docs/TWIN_PROTOCOL.md`.
- **Satellite reflectors** — lightweight relay instances that connect to a parent
  reflector instead of joining the full mesh, reducing configuration overhead for
  large deployments
- **Per-trunk TG filters** — `BLACKLIST_TGS`, `ALLOW_TGS`, `TG_MAP` (with
  explicit-route bypass of the prefix routing check), and `PEER_ID` on each
  `[TRUNK_x]` section, with a shared filter syntax (exact, `24*` prefix,
  `2427-2438` range). Reloadable at runtime via the PTY admin channel
- **Node-list broadcasting** — on local client login / logout / TG change the
  reflector debounces (500 ms) and emits a `MsgPeerNodeList` to every trunk
  peer (and, when configured, the `[TWIN]` partner) carrying callsign +
  current TG + optional lat/lon/QTH. Inbound lists from trunk peers or twin
  partners are republished via MQTT under `nodes/<peer_id>`, Redis under
  `live:peer_node:…`, and surfaced in `/status` under
  `trunks[SECTION].nodes` / `twin.nodes`. The local list is published on
  `nodes/local`
- **PTY admin channel** — live commands written to `/dev/shm/reflector_ctrl`:
  `TRUNK MUTE|UNMUTE <section> <callsign>` (applied on the inbound audio path),
  `TRUNK RELOAD [<section>]` (re-reads filters without restart),
  `TRUNK STATUS [<section>]`, plus CFG GET/SET and the existing certificate
  commands
- **HTTP `/status` endpoint** — JSON status with trunk state,
  active talkers, satellite connections, per-trunk muted callsigns,
  Redis metrics (when enabled), and static configuration
- **MQTT publishing** — real-time event-driven updates (talker, client,
  trunk state, receiver signal levels, node lists) to an external MQTT broker,
  plus configurable periodic full status dumps. Optional `MQTT_NAME` inserts
  a path component so multiple reflectors can share a `TOPIC_PREFIX`
- **Optional Redis-backed config store** — when `[REDIS]` is configured,
  users/passwords, cluster TGs, per-trunk filters, and trunk peers themselves
  are read from Redis. A web dashboard can add/remove users and trunk peers,
  push filter changes, and subscribe to live state (`live:client:*`,
  `live:talker:*`, `live:trunk:*`) without restarting the reflector. See
  [`docs/REDIS.md`](docs/REDIS.md)
- SvxLink client nodes are **unmodified** — they connect to their local
  reflector as normal and are unaware of the trunk

---

## How TG ownership works

TG numbers work like telephone numbers. The reflector that owns a TG prefix is
the authoritative home for every TG in that prefix group:

```
Reflector 1  (LOCAL_PREFIX=1)   →  TGs  1, 10, 11, 100, 1000, 12345, …
Reflector 2  (LOCAL_PREFIX=2)   →  TGs  2, 20, 21, 200, 2000, 25000, …
Reflector 3  (LOCAL_PREFIX=3)   →  TGs  3, 30,  …
```

When a client on Reflector 1 joins TG 25, Reflector 1 forwards the conversation
over its trunk to Reflector 2 (which owns the "2" prefix). Clients on both
reflectors hear the same audio on TG 25 with no extra configuration.

---

## Build

Required: libsigc++, OpenSSL, libjsoncpp, libpopt.
Required by default (opt out with the CMake flags below): libmosquitto, libhiredis.
Optional codecs: libopus, libgsm, libspeex.

```bash
cd geureflector
cmake -S src -B build -DLOCAL_STATE_DIR=/var
cmake --build build
# binary at build/bin/svxreflector
```

To skip the MQTT publisher or the Redis backend at compile time (e.g. on a
host without `libmosquitto-dev` or `libhiredis-dev`):

```bash
cmake -S src -B build -DLOCAL_STATE_DIR=/var -DWITH_MQTT=OFF
cmake -S src -B build -DLOCAL_STATE_DIR=/var -DWITH_REDIS=OFF
```

See [`docs/INSTALL.md`](docs/INSTALL.md) for the full step-by-step.

---

## Testing

Integration tests spin up Docker reflector meshes and verify trunk routing,
audio delivery, twin failover, and protocol behavior:

```bash
cd tests && bash run_tests.sh
```

Requires Docker and Python 3.7+. The script builds the images, starts the
meshes, and runs 89 automated tests across six suites (trunk, MQTT deltas,
satellite secrets, logging, twin, and routable prefixes). When run in a
terminal, the trunk suite ends with an interactive prompt where you can enter
any TG number and see which reflector it routes to (verified via container
logs). See [`tests/TESTS.md`](tests/TESTS.md) for the full breakdown.

The mesh topology is defined in `tests/topology.py` — edit prefixes, cluster
TGs, or add reflectors there, and `run_tests.sh` regenerates all configs
automatically.

---

## Configuration

GeuReflector uses the same `svxreflector.conf` format as SvxReflector with two
additions.

### 1. Declare this reflector's TG prefix

Add `LOCAL_PREFIX` to the `[GLOBAL]` section. A comma-separated list is accepted
when a single reflector owns multiple prefix groups:

```ini
[GLOBAL]
LISTEN_PORT=5300
LOCAL_PREFIX=1              # owns TGs 1, 10, 100, 1000, ...
# LOCAL_PREFIX=11,12,13     # multiple prefixes on one instance
# TRUNK_LISTEN_PORT=5302    # trunk server port (default 5302)
```

### 2. Cluster TGs (optional)

Cluster TGs are broadcast to **all** trunk peers regardless of prefix ownership,
like BrandMeister's nationwide talk groups. Any reflector can originate a
transmission on a cluster TG and all other reflectors in the mesh will hear it.

```ini
[GLOBAL]
CLUSTER_TGS=222,2221,91    # comma-separated list of cluster TG numbers
```

Each reflector owner chooses which cluster TGs to subscribe to. A reflector only
sends and accepts traffic for cluster TGs listed in its own `CLUSTER_TGS`. If
reflector A subscribes to TG 222 but reflector B does not, A will send TG 222
traffic to B, but B will ignore it — this is normal operation, not a
misconfiguration. Only reflectors that both subscribe to a given cluster TG will
exchange audio for it.

Cluster TG numbers must not overlap with any `LOCAL_PREFIX` or `REMOTE_PREFIX`.

**Note:** Satellite links are unaffected by cluster TG configuration — by
default they forward all TGs in both directions. A satellite can narrow
that scope with `SATELLITE_FILTER` (see [Satellite reflectors](#satellite-reflectors)).

### 3. Trunk debug logging (optional)

Enable verbose trunk logging to diagnose connection issues:

```ini
[GLOBAL]
TRUNK_DEBUG=1
```

When enabled, the reflector logs detailed information about every trunk
connection state change, including:

- Connection and disconnection events with full state (both directions'
  hello/heartbeat counters, whether the other direction is up)
- Heartbeat countdown warnings as the RX counter approaches timeout
- Every non-heartbeat frame received, with direction (IB/OB), type, and size
- Send path decisions (outbound, fallback to inbound, or dropped)
- Trunk server peer matching: which sections matched or mismatched on HMAC
  secret and prefix, making configuration errors immediately visible

**Warning:** `TRUNK_DEBUG` logs **every audio frame** received over trunk. During
active transmissions this means dozens of lines per second per TG per trunk link
(e.g. ~50 lines/sec with 20ms Opus framing). On a busy reflector with multiple
simultaneous talkers across several trunk peers, this can produce hundreds of
log lines per second and rapidly fill disk. **Do not leave `TRUNK_DEBUG=1`
enabled in production.** Use it only to diagnose a specific issue, then disable
it immediately.

### 4. Add a trunk section per peer

```ini
[TRUNK_AB]
HOST=reflector-b.example.com
PORT=5302             # trunk port (default 5302, separate from client port 5300)
SECRET=shared_secret
REMOTE_PREFIX=2       # peer owns TGs 2, 20, 200, 2000, ...
# REMOTE_PREFIX=11,12,13  # comma-separated list accepted here too
```

Repeat for each peer (`[TRUNK_AC]`, `[TRUNK_AD]`, …).

**Important:** The `[TRUNK_x]` section name must be **identical on both sides** of
the link. Both sysops must agree on a shared section name before configuring the
link. For example, if reflectors A and B want to link, both configs must use the
same name (e.g. `[TRUNK_AB]`). A connection from a peer whose section name does
not match any local section will be rejected.

### 5. Routable prefixes — hierarchical and non-full-mesh topologies (optional)

In a standard full mesh every pair of reflectors has a direct trunk link, so
each reflector can reach any TG via `REMOTE_PREFIX` alone.  Some deployments
cannot or should not use a full mesh:

- A **regional leaf** with a single uplink to a national backbone has no
  direct path to sibling regions — it needs a default route via the parent.
- A **transit backbone** node between two meshes may carry traffic for prefix
  groups it does not own and that are not adjacent in `REMOTE_PREFIX`.

`ROUTABLE_PREFIXES` (per `[TRUNK_x]` section, comma-separated) declares
additional TG prefixes that are reachable *via* this peer beyond what
`REMOTE_PREFIX` already covers.

**Explicit prefixes** (e.g. `263`) extend the mesh-wide prefix set so the
reflector will accept those TGs inbound and transit them onward toward the
owner via the existing gateway fanout (`shouldRelayInbound`).  Each intermediate
hop along a chain must declare the same routable prefix on the appropriate trunk
— there is no automatic propagation.

**`*` wildcard** acts as a default route for a single-uplink regional leaf: local
clients can reach any TG via that uplink, and the return audio is accepted back.
`*` is never relayed between trunks (loop-safe) and is not added to the
mesh-wide prefix set, so it is invisible to gateway forwarding.  A `*` is a
zero-length, lowest-precedence match — it never overrides a real prefix.  A
startup `WARNING` is logged when `*` is set on a reflector that has more than
one trunk, since that topology is outside the intended single-uplink leaf
pattern.

**Blacklist precedence:** `BLACKLIST_TGS` is evaluated before any routable
logic — a blacklisted TG is never forwarded, accepted, or advertised as
interest, regardless of `ROUTABLE_PREFIXES`.

**Delegation note:** a routable prefix more specific than this reflector's own
`LOCAL_PREFIX` intentionally delegates that sub-range elsewhere
(longest-prefix-match: the more specific routable prefix wins).  Point such a
prefix at the reflector that actually owns those TGs.

**Runtime note:** the mesh-wide prefix set used for transit is built at startup,
so changing `ROUTABLE_PREFIXES` takes full effect on reflector restart (as with
`REMOTE_PREFIX`).

Example — four-node chain where `leaf` (prefix `222100`) needs to reach TG
`263xx` owned by `far` (prefix `263`), transiting through both `natlit`
(prefix `222`) and `natlde` (prefix `262`):

```ini
# On leaf — default route via the only uplink toward natlit
[TRUNK_LEAF_NATLIT]
HOST=natlit.example.com
SECRET=secret_leaf_natlit
REMOTE_PREFIX=222
ROUTABLE_PREFIXES=*

# On natlit — explicit transit toward 263 via the natlde link
[TRUNK_NATLIT_NATLDE]
HOST=natlde.example.com
SECRET=secret_natlit_natlde
REMOTE_PREFIX=262
ROUTABLE_PREFIXES=263

# On natlde — transit 263 back toward natlit (return path).
# The section name MUST be identical on both ends of a link (see section 4),
# so natlde's trunk to natlit reuses the same [TRUNK_NATLIT_NATLDE] name.
[TRUNK_NATLIT_NATLDE]
HOST=natlit.example.com
SECRET=secret_natlit_natlde
REMOTE_PREFIX=222
ROUTABLE_PREFIXES=263

[TRUNK_NATLDE_FAR]
HOST=far.example.com
SECRET=secret_natlde_far
REMOTE_PREFIX=263
# far owns 263 natively — no ROUTABLE_PREFIXES needed on this last hop
```

The `*` wildcard on leaf's uplink also handles the return leg: leaf advertises
interest in `263xx` toward natlit via `MsgPeerTgInterest`, and — on top of that
— accepts the returning audio via the wildcard match (`matchesRoutable`) on its
inbound-accept gate, since `*` is excluded from the mesh prefix set and
`hasPrefixRoute` alone cannot serve this role.

### Network requirements

**Trunk links** require **mutual reachability**: each reflector both listens for
inbound trunk connections and connects outbound to every peer.  Both sides
attempt to connect simultaneously; outbound and inbound connections operate
independently and can both be active at the same time.  When sending, the
outbound connection is preferred with inbound as fallback.  All reflectors in
the mesh need a static IP (or
stable DNS name) and the trunk port (default 5302, configurable via
`TRUNK_LISTEN_PORT`) open for inbound TCP connections.

**Satellite links** are **one-way**: the satellite connects outbound to the
parent. Only the parent needs a static IP and its satellite port (default 5303)
open. The satellite itself does not need a static IP or any open ports — it can
run behind NAT, just like a regular SvxLink client node.

| Port | Protocol | Direction | Required on |
|------|----------|-----------|-------------|
| 5300 | TCP+UDP | Inbound | All reflectors (client connections) |
| 5302 | TCP | Inbound | All trunk mesh reflectors (peer connections) |
| 5303 | TCP | Inbound | Parent reflectors accepting satellites |
| — | TCP | Outbound | Satellites (no inbound ports needed) |

### Full-mesh example — three reflectors

**Reflector A** — owns prefix "1":
```ini
[GLOBAL]
LOCAL_PREFIX=1

[TRUNK_AB]
HOST=reflector-b.example.com
SECRET=secret_ab
REMOTE_PREFIX=2

[TRUNK_AC]
HOST=reflector-c.example.com
SECRET=secret_ac
REMOTE_PREFIX=3
```

**Reflector B** — owns prefix "2":
```ini
[GLOBAL]
LOCAL_PREFIX=2

[TRUNK_AB]
HOST=reflector-a.example.com
SECRET=secret_ab
REMOTE_PREFIX=1

[TRUNK_BC]
HOST=reflector-c.example.com
SECRET=secret_bc
REMOTE_PREFIX=3
```

**Reflector C** — owns prefix "3":
```ini
[GLOBAL]
LOCAL_PREFIX=3

[TRUNK_AC]
HOST=reflector-a.example.com
SECRET=secret_ac
REMOTE_PREFIX=1

[TRUNK_BC]
HOST=reflector-b.example.com
SECRET=secret_bc
REMOTE_PREFIX=2
```

Both sides of each trunk link must use the **same section name** and share the
same `SECRET`. The sysops of both reflectors agree on the section name and
secret when setting up the link.

### Satellite reflectors

A satellite is a lightweight reflector that connects to a parent reflector
instead of joining the trunk mesh. Clients connect to the satellite normally;
it relays everything to the parent. The parent is unchanged — it still
participates in the trunk mesh as before.

```
          ┌──── Full mesh (unchanged) ────┐
          │                               │
    ┌─────┴─────┐                   ┌─────┴─────┐
    │ Refl. 01  │◄── trunk ──────►│ Refl. 02  │
    └─────┬─────┘                   └───────────┘
     ┌────┼────┐
     ▼    ▼    ▼
   Sat A Sat B Sat C     ← satellites of Reflector 01
```

**Parent side** — add a `[SATELLITE]` section to accept inbound satellites:

```ini
[SATELLITE]
LISTEN_PORT=5303
SECRET=regional_satellite_secret
```

`SECRET=` is the shared fallback used by every satellite. To pin a
satellite to its own secret (so that compromising the fallback cannot
impersonate it, and rotating one satellite does not require touching
the others), add per-id entries alongside:

```ini
[SATELLITE]
LISTEN_PORT=5303
SECRET=regional_satellite_secret
SECRET_alpha=secret_for_alpha
SECRET_bravo-east=secret_for_bravo_east
```

The id after `SECRET_` is matched case-sensitively against the
satellite's `SATELLITE_ID`. Allowed id characters: `[A-Za-z0-9-]+`
(alphanumeric + dash; **no underscore**). Malformed keys are logged and
ignored at startup.

Lookup rule when a satellite connects:

1. If `SECRET_<id>=` is set for the satellite's id, verify against
   that value. **Mismatch rejects the connection — there is no
   fallback once an id is pinned.**
2. Otherwise, fall back to `SECRET=` if set.
3. Otherwise, reject.

Keeping `SECRET=` alongside the per-id entries is backward-compatible:
existing satellites with no per-id entry continue to authenticate via
the default.

**Satellite side** — set `SATELLITE_OF` in `[GLOBAL]` instead of
`LOCAL_PREFIX` and `[TRUNK_x]` sections:

```ini
[GLOBAL]
SATELLITE_OF=reflector-01.example.com
SATELLITE_PORT=5303
SATELLITE_SECRET=regional_satellite_secret
SATELLITE_ID=my-satellite
# Optional — restrict which TGs this satellite cares about (bidirectional).
# Syntax: exact "26200", prefix "24*", range "2427-2438", comma-separated.
#SATELLITE_FILTER=24*,262*
```

A satellite does not set `LOCAL_PREFIX`, `REMOTE_PREFIX`, `CLUSTER_TGS`, or any
`[TRUNK_x]` sections. It inherits its parent's identity. Remote reflectors in
the mesh see satellite clients as if they were connected directly to the parent.

**Default: no TG filtering.** Unlike trunk links (which filter by prefix and
cluster TG), a satellite link by default forwards **all** audio and talker
signaling in both directions — every TG active on the parent is heard on the
satellite and vice versa, regardless of `CLUSTER_TGS` or prefix configuration.

**Opt-in TG scope via `SATELLITE_FILTER`.** Setting `SATELLITE_FILTER` on the
satellite side restricts the link to a chosen set of TGs in both directions:
the satellite suppresses its own outbound events for non-matching TGs, and
advertises the filter to the parent (via `MsgPeerFilter`, type 122) so the
parent also stops forwarding non-matching TGs back. Empty or absent means no
filtering. The filter uses the shared TG-filter syntax (exact, `24*` prefix,
`2427-2438` range, comma-separated).

**Observability.** The currently active filter appears under
`/status.satellites[<id>].filter` on the parent — and therefore also in
the retained MQTT `status` topic and the Redis `live:satellite:<id>`
snapshot, since those surfaces reserialize the same JSON. The key is
omitted when no filter is set.

Port `5303` is the default satellite port (separate from client port `5300` and
trunk port `5302`).

### MQTT publishing (optional)

Publishes real-time events and periodic status to an external MQTT broker,
eliminating the need to poll the `/status` endpoint. See
[`docs/MQTT.md`](docs/MQTT.md) for the full topic structure, payload format,
and TLS configuration.

```ini
[MQTT]
HOST=mqtt.example.com
PORT=1883
USERNAME=reflector
PASSWORD=secret
TOPIC_PREFIX=svxreflector/myreflector
STATUS_INTERVAL=30000
```

Omit the `[MQTT]` section entirely to disable — zero overhead when not
configured.

---

## HTTP Status

Enable with `HTTP_SRV_PORT=8080` in `[GLOBAL]`.

### `GET /status`

Returns live state (nodes, trunk connections, active talkers, satellites) and
static configuration in a single response:

```json
{
  "version": "1.3.99.13+trunk.twin2",
  "mode": "reflector",
  "local_prefix": ["1"],
  "listen_port": "5300",
  "http_port": "8080",
  "nodes": { ... },
  "cluster_tgs": [222, 2221, 91],
  "trunks": {
    "TRUNK_2": {
      "host": "reflector-b.example.com",
      "port": 5302,
      "connected": true,
      "local_prefix": ["1"],
      "remote_prefix": ["2"],
      "active_talkers": {
        "222": "IW1GEU",
        "25": "SM0ABC"
      },
      "muted": [],
      "nodes": [
        {"callsign": "SM0ABC", "tg": 25},
        {"callsign": "SM1DEF", "tg": 2}
      ]
    }
  },
  "twin": {
    "host": "reflector-partner.example.com",
    "port": 5304,
    "connected": true,
    "outbound_connected": true,
    "outbound_hello": true,
    "inbound_connected": true,
    "inbound_hello": true,
    "local_prefix": "1",
    "peer_id": "TWIN",
    "nodes": [
      {"callsign": "IW1XYZ", "tg": 1234}
    ]
  },
  "satellites": {
    "my-satellite": {
      "id": "my-satellite",
      "authenticated": true,
      "active_tgs": [1, 100]
    }
  },
  "satellite_server": {
    "listen_port": "5303",
    "connected_count": 2
  }
}
```

`active_talkers` lists TGs with an active remote talker at query time (both
prefix-based and cluster TGs). `trunks[SECTION].nodes` carries the roster
received from each trunk peer; `twin.nodes` carries the partner's roster
when a `[TWIN]` section is configured. `twin`, `satellites`, and
`satellite_server` appear only when applicable.

---

## Live user/password reload

Users and passwords can be updated at runtime **without restarting** via the
command PTY interface (enabled by default). PTY commands only modify the
**in-memory** configuration — to persist across reboots, also update the config
file on disk:

```bash
# 1. Apply immediately (in-memory, takes effect now)
echo "CFG USERS SM0ABC MyGroup" > /dev/shm/reflector_ctrl
echo "CFG PASSWORDS MyGroup s3cretP@ss" > /dev/shm/reflector_ctrl

# 2. Persist to disk (survives reboot)
cat >> /etc/svxlink/svxreflector.conf <<'EOF'

[USERS]
SM0ABC=MyGroup

[PASSWORDS]
MyGroup="s3cretP@ss"
EOF
```

Both steps are needed: the PTY gives you instant activation, the config file
gives you persistence. If you only edit the file, changes take effect at next
restart.

The PTY path is set by `COMMAND_PTY` in `[GLOBAL]` (default
`/dev/shm/reflector_ctrl`).

---

## Documentation

- [`docs/CONCEPTS.md`](docs/CONCEPTS.md) — **start here after the glossary**: how
  TG ownership, owner-relay, peer interest, cluster TGs, and talker arbitration
  fit together, with a worked three-way-QSO example
- [`docs/INSTALL.md`](docs/INSTALL.md) — how to build and install GeuReflector
  as a drop-in replacement for an existing SvxReflector installation
- [`docs/DOCKER.md`](docs/DOCKER.md) — running GeuReflector in a Docker container
  as a drop-in replacement for an existing SvxReflector installation
- [`docs/PEER_PROTOCOL.md`](docs/PEER_PROTOCOL.md) — full wire protocol
  specification: message format, handshake sequence, talker arbitration
  tie-breaking, heartbeat, and the complete message type table
- [`docs/TWIN_PROTOCOL.md`](docs/TWIN_PROTOCOL.md) — twin (HA-pair) protocol:
  shared-prefix pairing, sticky per-transmission socket selection, failover,
  cross-twin arbitration, and the external-peer `PAIRED=1` requirement
- [`docs/TOPOLOGY_EXAMPLES.md`](docs/TOPOLOGY_EXAMPLES.md) — visual
  cookbook of the five supported topology shapes (international
  full-mesh, national mesh joined at gateway, satellite tree, hybrid,
  HA twin pair) with rendered diagrams; PDF version in
  [`docs/TOPOLOGY_EXAMPLES.pdf`](docs/TOPOLOGY_EXAMPLES.pdf)
- [`docs/DEPLOYMENT_ITALY.md`](docs/DEPLOYMENT_ITALY.md) — complete national
  deployment example for Italy (20 regions, full mesh)
- [`docs/DEPLOYMENT_ITALY_IT.md`](docs/DEPLOYMENT_ITALY_IT.md) — same document
  in Italian
- [`docs/WW_DEPLOYMENT.md`](docs/WW_DEPLOYMENT.md) — worldwide deployment
  example (25 countries, full mesh, DMR MCC-based TG numbering)
- [`docs/MCC_COUNTRY_CODES.md`](docs/MCC_COUNTRY_CODES.md) — reference table of
  3-digit Mobile Country Codes (ITU E.212) for anchoring `LOCAL_PREFIX` on your
  country's MCC
- [`docs/MQTT.md`](docs/MQTT.md) — MQTT publishing: topic structure, payload
  format, configuration reference, and TLS setup
- [`docs/REDIS.md`](docs/REDIS.md) — Redis-backed config store: schema,
  dashboard operations, pub/sub, migration from .conf
- [`docs/LOGGING.md`](docs/LOGGING.md) — leveled, per-subsystem,
  live-reloadable logger: `LOG=` config, PTY commands, Docker vs.
  `--logfile` deployments, migration from `TRUNK_DEBUG`
- [`docs/TROUBLESHOOTING.md`](docs/TROUBLESHOOTING.md) — symptom-first checklists
  for trunks that won't connect, audio that won't cross, satellite/twin issues,
  and flapping links
- [`docs/MESSAGING_IDEAS.md`](docs/MESSAGING_IDEAS.md) — ideas for consuming
  MQTT events via Telegram, SMS, Discord, webhooks, dashboards, and more
- [`docs/DESIGN_SATELLITE_AND_CLUSTER.md`](docs/DESIGN_SATELLITE_AND_CLUSTER.md) — design
  rationale for satellite reflectors and cluster TGs
- [`docs/JAY-additions.md`](docs/JAY-additions.md) — features merged from
  jayReflector by DJ1JAY / FM-Funknetz: per-trunk filters, TG mapping,
  PEER_ID, MQTT_NAME, node-list exchange, PTY mute/reload/status
- [`tests/TESTS.md`](tests/TESTS.md) — integration test suite documentation:
  topology, test cases, harness components, and how to run

---

## License

GNU General Public License v2 or later — same as SvxLink upstream.

---

> **Tip:** Get the most out of this trunk reflector edition with
> [audric/SvxReflectorDashboard](https://github.com/audric/SvxReflectorDashboard) — an all-in-one
> SvxLink management suite with real-time monitoring, configuration, and control
> for your entire reflector mesh.
