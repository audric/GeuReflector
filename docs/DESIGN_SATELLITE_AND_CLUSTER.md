# Design: Regional Satellites & Nationwide Cluster TGs

Two extensions to GeuReflector's trunk architecture, both now fully
implemented.

---

## 1. Regional Satellite Reflectors

### Problem

Full mesh works well for 5-10 reflectors but scales as N*(N-1)/2 connections.
Italy's 20-region deployment already needs 190 trunk links. A country with 50
regions would need 1225. Worse, many small regions don't justify the operational
overhead of managing 49 trunk peer configurations.

### Concept

A **satellite** is a lightweight reflector instance that connects to a single
parent reflector instead of joining the full mesh. Clients connect to the
satellite normally; the satellite relays everything to its parent. The parent
reflector is unchanged — it still participates in the trunk mesh as before, and
additionally accepts inbound satellite connections.

```
              ┌──────── Nationwide full mesh (unchanged) ────────┐
              │                                                  │
        ┌─────┴─────┐                                      ┌─────┴─────┐
        │ Refl. 01  │◄── trunk ──────────────────────────►│ Refl. 02  │
        └─────┬─────┘                                      └───────────┘
     ┌────────┼────────┐
     ▼        ▼        ▼
  Sat 01a  Sat 01b  Sat 01c          (satellites of Reflector 01)
     │        │        │
  clients  clients  clients
```

Satellites are transparent to the rest of the mesh — remote reflectors see
satellite clients as if they were connected directly to the parent.

### Config changes

On the **parent reflector** (existing reflector, no role change needed):

```ini
[GLOBAL]
LOCAL_PREFIX=01

# Existing trunk config unchanged
[TRUNK_01_02]
HOST=reflector-02.example.com
SECRET=secret_01_02
REMOTE_PREFIX=02

# NEW: accept satellite connections
[SATELLITE]
LISTEN_PORT=5303
SECRET=regional_satellite_secret
```

On the **satellite**:

```ini
[GLOBAL]
SATELLITE_OF=reflector-01.example.com
SATELLITE_PORT=5303
SATELLITE_SECRET=regional_satellite_secret
```

A satellite does **not** set `LOCAL_PREFIX`, `REMOTE_PREFIX`, or any
`[TRUNK_x]` sections. It inherits its parent's prefix identity. It does not
connect to other reflectors or other satellites.

### Implementation

#### New class: `SatelliteLink`

Similar to `TrunkLink` but with different semantics:

| Aspect | TrunkLink (peer-to-peer) | SatelliteLink |
|--------|-------------------------|---------------|
| Topology | Symmetric mesh peers | Asymmetric — parent is authority |
| TG routing | Prefix-based (`isSharedTG`/`isOwnedTG`) + cluster (`isClusterTG`) | **No filtering** — all TGs forwarded unconditionally |
| Talker arbitration | Nonce tie-break | Parent always wins |
| Who initiates | Both sides connect to each other | Satellite connects to parent |
| Audio path | Only prefix-matched + cluster TGs | All TGs active on either side |
| `CLUSTER_TGS` config needed | Yes, must match on both sides | Not required |

#### Parent-side changes (`Reflector.cpp`)

1. **New TCP server** on the satellite port (5303), separate from the client
   server (5300) and trunk mesh (5302).

2. **`SatelliteLink` instances** created for each inbound satellite connection
   (dynamically on accept, like `ReflectorClient`).

3. **Audio forwarding**: when a local client or trunk peer starts talking on any
   TG, the parent also sends `MsgTrunkTalkerStart` + `MsgTrunkAudio` to all
   connected satellites. When a satellite client talks, the parent treats it
   identically to a local client — forwards to trunk peers via `isSharedTG` as
   normal.

4. **TGHandler**: satellite talkers are registered as regular local talkers (the
   satellite is transparent). The parent's `TGHandler` sees them as
   `ReflectorClient` objects proxied through the satellite.

#### Satellite-side changes

The satellite is a **thin relay**:

1. Clients connect to it on port 5300 as normal.
2. All talker events and audio are forwarded to the parent over the satellite
   connection.
3. All talker events and audio received from the parent are broadcast to local
   clients.
4. No `TGHandler` prefix logic — the satellite doesn't decide TG ownership.

#### Wire protocol

Reuse the existing trunk message types (115-120). The satellite handshake
(`MsgTrunkHello`) includes a flag field so the parent knows to treat the
connection as a satellite rather than a mesh peer.

Add one field to `MsgTrunkHello`:

```cpp
uint8_t m_role;  // 0 = PEER (default, backward-compatible), 1 = SATELLITE
```

#### Key files to modify

| File | Change |
|------|--------|
| `Reflector.cpp` | Add satellite TCP server, accept satellite connections |
| `SatelliteLink.cpp` (new) | Parent-side satellite connection handler |
| `SatelliteClient.cpp` (new) | Satellite-side parent connection + local relay |
| `ReflectorMsg.h` | Add `m_role` to `MsgTrunkHello` |
| `TrunkLink.cpp` | No changes — mesh unchanged |
| `svxreflector.conf.in` | Add `[SATELLITE]`, `SATELLITE_OF` settings |

#### Failure modes

- **Satellite loses parent connection**: local clients stay connected but can't
  reach other regions or other clients on the parent. Satellite retries
  automatically (same as TrunkLink reconnect logic).
- **Parent goes down**: all its satellites are isolated. Other reflectors in the
  mesh see the parent's trunk links drop — same as today.
- **Satellite is optional**: a region with only a few nodes doesn't need
  satellites. Zero overhead when not used.

---

## 2. Nationwide Cluster TGs (BrandMeister-style)

### Problem

The current prefix model means every TG is **owned by exactly one reflector**.
TG 22201 belongs to whoever owns prefix `01`. This works well for regional TGs
but doesn't support the BrandMeister "cluster TG" pattern — a talk group that
is **shared equally by all reflectors** with no single owner, where keying up
on any reflector is heard on every other reflector.

Examples from BrandMeister:

- TG 91 = Worldwide cluster
- TG 222 = Italy nationwide cluster
- TG 2221 = North Italy cluster

### Concept

Add a **cluster TG** category that bypasses prefix routing. When a client keys
up on a cluster TG at any reflector, that reflector broadcasts the audio to
**all** trunk peers, regardless of prefix ownership. Any reflector can originate
a cluster TG transmission.

```
    Reflector 01          Reflector 02          Reflector 03
    ┌──────────┐          ┌──────────┐          ┌──────────┐
    │ Client A │          │ Client B │          │ Client C │
    │ TG 222   │          │ TG 222   │          │ TG 222   │
    └────┬─────┘          └────┬─────┘          └────┬─────┘
         │     ◄── cluster broadcast ──►              │
         └──────────────── all hear A ────────────────┘
```

Compared to prefix-based TGs:

| Aspect | Prefix TG (e.g. 22201) | Cluster TG (e.g. 222) |
|--------|----------------------|------------------------|
| Owner | Single reflector (prefix `01`) | None — shared by all |
| Routing | Only to/from owner's trunk peer | Broadcast to all trunk peers |
| Who can originate | Any client, but routed through owner | Any client, handled locally |
| Arbitration | Nonce tie-break between 2 peers | Nonce tie-break between all peers |

### Config changes

New `[GLOBAL]` key `CLUSTER_TGS`:

```ini
[GLOBAL]
LOCAL_PREFIX=01
# Cluster TGs: broadcast to all trunk peers regardless of prefix
# Comma-separated list of individual TGs
CLUSTER_TGS=222,2221,2222,91
```

Each reflector owner chooses which cluster TGs to subscribe to. The cluster TG
check is applied independently on both the sending and receiving side of each
trunk link. If a cluster TG is declared on one side but not the other, the
receiving side silently ignores the traffic — this is normal operation, not a
misconfiguration. Only reflectors that both subscribe to a given cluster TG will
exchange audio for it. A TG number cannot be both a cluster TG and match a
`LOCAL_PREFIX`/`REMOTE_PREFIX`.

**Note:** Satellite links are not affected by `CLUSTER_TGS` — they forward all
TGs unconditionally regardless of configuration.

### Implementation

#### TG classification (`TrunkLink.cpp` / new helper)

Today, `isSharedTG()` checks if a TG matches the remote peer's prefix. Add a
parallel check:

```cpp
// In Reflector or a shared utility
bool isClusterTG(uint32_t tg) const
{
  return m_cluster_tgs.count(tg) > 0;
}
```

Modify the routing decision from:

```
if isSharedTG(tg) → send to that specific peer
```

to:

```
if isClusterTG(tg) → send to ALL peers
else if isSharedTG(tg) → send to that specific peer
```

#### Changes to `TrunkLink`

`onLocalTalkerStart`, `onLocalAudio`, `onLocalFlush`, `onLocalTalkerStop`
currently filter with `isSharedTG(tg)`. Add `|| isClusterTG(tg)`:

```cpp
void TrunkLink::onLocalTalkerStart(uint32_t tg, const std::string& callsign)
{
  if (!m_con.isConnected() || !m_hello_received ||
      (!isSharedTG(tg) && !m_reflector->isClusterTG(tg)))
  {
    return;
  }
  sendMsg(MsgTrunkTalkerStart(tg, callsign));
}
```

Similarly for inbound: `handleMsgTrunkTalkerStart` and the other receive
handlers use `isOwnedTG` (checks both local and remote prefix) instead of
`isSharedTG`, combined with `|| isClusterTG`:

```cpp
if (!isOwnedTG(tg) && !m_reflector->isClusterTG(tg))
{
  return;
}
```

#### Multi-peer arbitration

With prefix TGs, a conflict only involves 2 reflectors (local + the owner).
Cluster TGs can have N-way conflicts: 3 reflectors might all have a local
talker key up on TG 222 simultaneously.

The existing nonce tie-break still works because each pair of connected peers
resolves independently:

1. Reflector A (nonce 500) and Reflector B (nonce 300) both claim TG 222.
2. A-B link: B wins (lower nonce). A yields, sends TalkerStop to all its other
   peers.
3. Reflector C (nonce 700) also claimed TG 222 on the A-C and B-C links. Both
   A and B have lower nonces than C, so C yields on both links.
4. Net result: B (nonce 300) wins everywhere. Convergence takes one
   round-trip per link — same as today.

No protocol changes needed. The nonce comparison is already pairwise.

#### Changes to `Reflector.cpp`

The `onTalkerUpdated` callback currently iterates all trunk links and calls
`link->onLocalTalkerStart()`. No change needed — each `TrunkLink` now
individually decides whether to forward based on `isSharedTG || isClusterTG`.

Audio forwarding in `Reflector::onTalkerUpdated` already loops all links. Same
for `handleMsgTrunkAudio` -> `broadcastUdpMsg` on the receiving side.

#### Cluster TG validation at startup

During `initTrunkLinks()`, validate that no cluster TG matches any
`LOCAL_PREFIX` or `REMOTE_PREFIX`. This prevents ambiguity about whether a TG is
cluster-routed or prefix-routed:

```cpp
for (uint32_t tg : m_cluster_tgs)
{
  std::string s = std::to_string(tg);
  for (const auto& prefix : local_prefixes)
  {
    if (s.compare(0, prefix.size(), prefix) == 0)
    {
      cerr << "*** ERROR: Cluster TG " << tg
           << " conflicts with LOCAL_PREFIX " << prefix << endl;
      return false;
    }
  }
}
```

#### HTTP `/status` extension

Add a `cluster_tgs` array to the status JSON and include cluster TG talkers in
each trunk link's `active_talkers`:

```json
{
  "cluster_tgs": [222, 2221, 91],
  "trunks": {
    "TRUNK_01_02": {
      "active_talkers": {
        "222": "IW1GEU",
        "25": "SM0ABC"
      }
    }
  }
}
```

#### Key files to modify

| File | Change |
|------|--------|
| `Reflector.h` | Add `m_cluster_tgs` set, `isClusterTG()` method |
| `Reflector.cpp` | Parse `CLUSTER_TGS` config, validate no prefix overlap |
| `TrunkLink.cpp` | Add `isClusterTG` to routing checks in all 6 handlers |
| `svxreflector.conf.in` | Add `CLUSTER_TGS` example |

No new message types. No wire protocol changes. No new classes.

---

## Comparison

| | Satellite reflectors | Cluster TGs |
|---|---|---|
| **Solves** | Mesh scaling for large deployments | Shared nationwide channels |
| **Complexity** | New class + new TCP listener | ~50 lines of routing logic changes |
| **Wire protocol** | 1 new field in MsgTrunkHello | No changes |
| **Config** | `[SATELLITE]`, `SATELLITE_OF` | `CLUSTER_TGS=222,91` |
| **Risk** | Medium — new connection type | Low — additive routing check |
| **Recommended order** | Second | **First** (simpler, high value) |

### Suggested implementation order

1. **Cluster TGs first** — small code change, no new protocol, immediately
   useful for national channels.
2. **Satellite reflectors second** — larger feature, only needed when mesh
   size becomes a bottleneck (>10 reflectors).

---

## Combined example: Italy with both features

```
              ┌──── Nationwide full mesh (unchanged) ─────┐
              │        Cluster TGs: 222, 2221             │
              │                                           │
        ┌─────┴─────┐                              ┌─────┴─────┐
        │ Refl. 01  │◄── trunk (prefix 01↔02) ───►│ Refl. 02  │
        │ (LAZIO)   │     + cluster 222,2221       │ (SARDEGNA)│
        └─────┬─────┘                              └───────────┘
     ┌────────┼────────┐
     ▼        ▼        ▼
  Sat Roma Sat Viter Sat Latina        (satellites of Reflector 01)
  clients  clients   clients
```

- A client on Satellite Roma joins **TG 22201** (regional Lazio): heard by all
  Lazio clients (via parent reflector) and by all reflectors that have prefix
  `01` as a `REMOTE_PREFIX`.
- The same client joins **TG 222** (national cluster): heard by **every
  reflector** in the mesh, regardless of prefix.
- TG routing is unambiguous: cluster TGs are checked first, then prefix routing.
