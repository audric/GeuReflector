# GeuReflector

GeuReflector is a fork of [SvxReflector](https://github.com/sm0svx/svxlink)
extended with a **server-to-server trunk protocol** that lets multiple reflector
instances share talk groups (TGs) as independent parallel voice channels —
analogous to telephone trunk lines between telephone exchanges.

Original SvxReflector by Tobias Blomberg / SM0SVX.
Trunk extension by IW1GEU.

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
- **Full-mesh topology** — any number of reflectors can be trunked together; each
  pair has a direct link so no multi-hop routing is needed
- **Unlimited concurrent conversations** — the trunk TCP connection multiplexes
  all active TGs simultaneously; the only per-TG rule is one talker at a time
- **Independent talker arbitration** — each reflector arbitrates its own clients;
  the trunk adds a tie-breaking layer (random priority nonce) for simultaneous
  claims
- **Cluster TGs** — BrandMeister-style nationwide talk groups that are broadcast
  to all trunk peers regardless of prefix ownership
- **Satellite reflectors** — lightweight relay instances that connect to a parent
  reflector instead of joining the full mesh, reducing configuration overhead for
  large deployments
- **HTTP `/status` endpoint** — JSON status with trunk state,
  active talkers, satellite connections, and static configuration
- **MQTT publishing** — real-time event-driven updates (talker, client,
  trunk state, receiver signal levels) to an external MQTT broker, plus
  configurable periodic full status dumps
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

Requires the same dependencies as SvxReflector: libsigc++, OpenSSL, libjsoncpp,
libpopt, libmosquitto. Optional: libopus, libgsm, libspeex.

```bash
cd geureflector
cmake -S src -B build -DLOCAL_STATE_DIR=/var
cmake --build build
# binary at build/bin/svxreflector
```

---

## Testing

Integration tests spin up a 3-reflector Docker mesh and verify trunk routing,
audio delivery, and protocol behavior:

```bash
cd tests && bash run_tests.sh
```

Requires Docker and Python 3.7+. The script builds the images, starts the mesh,
runs 18 automated tests, then drops into an interactive prompt where you can
enter any TG number and see which reflector it routes to (verified via container
logs).

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

**Note:** Satellite links are unaffected by cluster TG configuration — they
forward all TGs unconditionally (see [Satellite reflectors](#satellite-reflectors)).

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

**Satellite side** — set `SATELLITE_OF` in `[GLOBAL]` instead of
`LOCAL_PREFIX` and `[TRUNK_x]` sections:

```ini
[GLOBAL]
SATELLITE_OF=reflector-01.example.com
SATELLITE_PORT=5303
SATELLITE_SECRET=regional_satellite_secret
SATELLITE_ID=my-satellite
```

A satellite does not set `LOCAL_PREFIX`, `REMOTE_PREFIX`, `CLUSTER_TGS`, or any
`[TRUNK_x]` sections. It inherits its parent's identity. Remote reflectors in
the mesh see satellite clients as if they were connected directly to the parent.

**No TG filtering:** Unlike trunk links (which filter by prefix and cluster TG),
satellite links forward **all** audio and talker signaling in both directions
without any TG filtering. The satellite is a transparent relay — every TG active
on the parent is heard on the satellite and vice versa, regardless of
`CLUSTER_TGS` or prefix configuration.

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
STATUS_INTERVAL=1000
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
  "version": "1.0.0",
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
      }
    }
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
prefix-based and cluster TGs). `satellites` and `satellite_server` appear only
when applicable.

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

- [`docs/INSTALL.md`](docs/INSTALL.md) — how to build and install GeuReflector
  as a drop-in replacement for an existing SvxReflector installation
- [`docs/DOCKER.md`](docs/DOCKER.md) — running GeuReflector in a Docker container
  as a drop-in replacement for an existing SvxReflector installation
- [`docs/TRUNK_PROTOCOL.md`](docs/TRUNK_PROTOCOL.md) — full wire protocol
  specification: message format, handshake sequence, talker arbitration
  tie-breaking, heartbeat, and the complete message type table
- [`docs/DEPLOYMENT_ITALY.md`](docs/DEPLOYMENT_ITALY.md) — complete national
  deployment example for Italy (20 regions, full mesh)
- [`docs/DEPLOYMENT_ITALY_IT.md`](docs/DEPLOYMENT_ITALY_IT.md) — same document
  in Italian
- [`docs/WW_DEPLOYMENT.md`](docs/WW_DEPLOYMENT.md) — worldwide deployment
  example (25 countries, full mesh, DMR MCC-based TG numbering)
- [`docs/MQTT.md`](docs/MQTT.md) — MQTT publishing: topic structure, payload
  format, configuration reference, and TLS setup
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
