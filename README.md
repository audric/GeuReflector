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
- **HTTP `/status` and `/config` endpoints** — JSON status with trunk state,
  active talkers, satellite connections, and static configuration
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
libpopt. Optional: libopus, libgsm, libspeex.

```bash
cd geureflector
cmake -S src -B build -DLOCAL_STATE_DIR=/var
cmake --build build
# binary at build/bin/svxreflector
```

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
LOCAL_PREFIX=1        # owns TGs 1, 10, 100, 1000, ...
# LOCAL_PREFIX=11,12,13   # multiple prefixes on one instance
```

### 2. Cluster TGs (optional)

Cluster TGs are broadcast to **all** trunk peers regardless of prefix ownership,
like BrandMeister's nationwide talk groups. Any reflector can originate a
transmission on a cluster TG and all other reflectors in the mesh will hear it.

```ini
[GLOBAL]
CLUSTER_TGS=222,2221,91    # comma-separated list of cluster TG numbers
```

All reflectors in the mesh should list the same cluster TGs. Cluster TG numbers
must not overlap with any `LOCAL_PREFIX` or `REMOTE_PREFIX`.

### 3. Add a trunk section per peer

```ini
[TRUNK_2]
HOST=reflector-b.example.com
PORT=5302             # trunk port (default 5302, separate from client port 5300)
SECRET=shared_secret
REMOTE_PREFIX=2       # peer owns TGs 2, 20, 200, 2000, ...
# REMOTE_PREFIX=11,12,13  # comma-separated list accepted here too
```

Repeat for each peer (`[TRUNK_3]`, `[TRUNK_4]`, …).

### Network requirements

**Trunk links** require **mutual reachability**: each reflector connects outbound
to every peer, so all reflectors in the mesh need a static IP (or stable DNS
name) and the trunk port (default 5302) open for inbound TCP connections.

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

[TRUNK_2]
HOST=reflector-b.example.com
SECRET=secret_ab
REMOTE_PREFIX=2

[TRUNK_3]
HOST=reflector-c.example.com
SECRET=secret_ac
REMOTE_PREFIX=3
```

**Reflector B** — owns prefix "2":
```ini
[GLOBAL]
LOCAL_PREFIX=2

[TRUNK_1]
HOST=reflector-a.example.com
SECRET=secret_ab
REMOTE_PREFIX=1

[TRUNK_3]
HOST=reflector-c.example.com
SECRET=secret_bc
REMOTE_PREFIX=3
```

**Reflector C** — owns prefix "3":
```ini
[GLOBAL]
LOCAL_PREFIX=3

[TRUNK_1]
HOST=reflector-a.example.com
SECRET=secret_ac
REMOTE_PREFIX=1

[TRUNK_2]
HOST=reflector-b.example.com
SECRET=secret_bc
REMOTE_PREFIX=2
```

Both sides of each trunk must point at each other and share the same `SECRET`.

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

A satellite does not set `LOCAL_PREFIX`, `REMOTE_PREFIX`, or any `[TRUNK_x]`
sections. It inherits its parent's identity. Remote reflectors in the mesh see
satellite clients as if they were connected directly to the parent.

Port `5303` is the default satellite port (separate from client port `5300` and
trunk port `5302`).

---

## HTTP Status

Enable with `HTTP_SRV_PORT=8080` in `[GLOBAL]`. Two endpoints are available:

### `GET /status` — live state

Returns nodes, trunk connection state, and active talkers:

```json
{
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
  }
}
```

`active_talkers` lists TGs with an active remote talker at query time (both
prefix-based and cluster TGs). `satellites` appears only when satellites are
connected.

### `GET /config` — static configuration

Returns the reflector's own configuration, useful for dashboards:

```json
{
  "mode": "reflector",
  "local_prefix": ["1"],
  "cluster_tgs": [222, 2221, 91],
  "listen_port": "5300",
  "http_port": "8080",
  "trunks": {
    "TRUNK_2": {
      "host": "reflector-b.example.com",
      "port": 5302,
      "remote_prefix": ["2"]
    }
  },
  "satellite_server": {
    "listen_port": "5303",
    "connected_count": 2
  }
}
```

When running in satellite mode:

```json
{
  "mode": "satellite",
  "listen_port": "5300",
  "satellite": {
    "parent_host": "reflector-01.example.com",
    "parent_port": "5303",
    "id": "my-satellite"
  }
}
```

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
- [`docs/DESIGN_SATELLITE_AND_CLUSTER.md`](docs/DESIGN_SATELLITE_AND_CLUSTER.md) — design
  rationale for satellite reflectors and cluster TGs

---

## License

GNU General Public License v2 or later — same as SvxLink upstream.
