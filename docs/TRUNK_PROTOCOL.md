# GeuReflector — Server-to-Server Trunk Protocol

## Overview

GeuReflector extends SvxReflector with a server-to-server trunk protocol that
allows two or more independent reflector instances to share talk groups (TGs)
as simultaneous, independent voice channels — analogous to telephone trunk lines.

TGs are routed by **numeric prefix**: each reflector owns all TGs whose decimal
string representation starts with a configured digit string.  For example, a
reflector configured with `LOCAL_PREFIX=1` is authoritative for TG 1, 10, 100,
1000, 12345, 19999999, etc.  This is analogous to E.164 telephone numbering
(country codes, area codes).

Multiple reflectors can be trunked together in a full mesh.  Each pair of
reflectors shares one `[TRUNK_x]` section name on both sides.  Each TG has exactly one
authoritative home reflector (the one whose prefix matches); all other
reflectors are transparent proxies for that TG.

> **Important:** Both reflectors in a trunk link **must use the same `[TRUNK_x]`
> section name**.  The section name is transmitted in the `MsgTrunkHello`
> handshake and both sides must agree on it for the connection to be matched to
> the correct `TrunkLink` instance.  The convention is to use sorted pair
> identifiers — for example, the link between reflector 1 and reflector 2 uses
> `[TRUNK_1_2]` on **both** sides.

Clients (SvxLink nodes) connect to their local reflector as normal and are
completely unaware of the trunk.  When a remote talker is active on a TG whose
prefix belongs to another reflector, local clients receive a standard
`MsgTalkerStart` with the remote node's callsign, indistinguishable from a
local talker.

---

## Architecture

```
 SvxLink nodes           SvxLink nodes           SvxLink nodes
      │                       │                       │
      ▼                       ▼                       ▼
 Reflector 1  ◄──trunk──►  Reflector 2  ◄──trunk──►  Reflector 3
  prefix "1"                prefix "2"                prefix "3"
      └──────────────────────────────────────────────►┘
                          (full mesh)
```

Each reflector runs one `TrunkLink` instance per configured peer and a trunk
TCP server (default port 5302, configurable via `TRUNK_LISTEN_PORT`).  Each
`TrunkLink` attempts an outbound connection to the peer while simultaneously
accepting inbound connections from the peer on the trunk server.  Only one
connection per peer is active at a time — when both directions connect
simultaneously, a deterministic tie-break selects which connection to keep
(see [Connection Conflict Resolution](#connection-conflict-resolution)).

---

## TG Ownership and Routing

A TG number belongs to the reflector whose `LOCAL_PREFIX` is a string prefix
of the TG's decimal representation.  When multiple prefixes match (because one
prefix is itself a prefix of another), the **longest matching prefix wins** —
analogous to longest-prefix-match in IP routing.

| TG number | Decimal string | Owned by |
|-----------|---------------|----------|
| 1         | "1"           | prefix "1" |
| 10        | "10"          | prefix "1" |
| 100       | "100"         | prefix "1" |
| 1234567   | "1234567"     | prefix "1" |
| 2         | "2"           | prefix "2" |
| 25        | "25"          | prefix "2" |
| 30        | "30"          | prefix "3" |

### Overlapping Prefixes

Prefixes may overlap — one prefix can be a prefix of another.  This enables
hierarchical TG numbering schemes.  For example, with prefixes `"12"` and
`"120"`:

| TG number | Matches        | Longest match | Owned by     |
|-----------|----------------|---------------|--------------|
| 125       | "12"           | "12" (len 2)  | prefix "12"  |
| 1205      | "12" and "120" | "120" (len 3) | prefix "120" |
| 12        | "12"           | "12" (len 2)  | prefix "12"  |
| 1200      | "12" and "120" | "120" (len 3) | prefix "120" |

At startup, each `TrunkLink` receives the complete set of all prefixes in the
mesh (local + all remotes).  `isSharedTG(tg)` finds the best matching remote
prefix for the peer, then verifies no other prefix in the mesh is a longer
match.  If a longer match exists, the TG belongs to that other reflector
instead.

In a full mesh of N reflectors, each `[TRUNK_x]` section on reflector A points
at reflector B and carries only TGs whose decimal string starts with B's prefix
(`REMOTE_PREFIX`).  There is never a chain of trunk relays — every peer-to-peer
hop is direct, and audio distribution across more than two reflectors is
handled by owner-relay (see below), not by daisy-chain forwarding.

### Owner-Relay Fanout

Prefix routing alone gets talker state and audio from a non-owner to the TG's
owner, and from the owner to its local clients.  But in a mesh of 3+
reflectors, the owner is also the only reflector that sees every non-owner's
traffic on that TG — so for every non-owner to hear every other, the owner
must fan out.

When `TrunkLink::handleMsgTrunkTalkerStart` / `handleMsgTrunkTalkerStop` /
`handleMsgTrunkAudio` / `handleMsgTrunkFlush` runs on the reflector that
**owns** the TG (`Reflector::isLocalTG(tg)`), after the existing local
delivery the event is re-forwarded to every **other** trunk peer through the
same per-link filter (`isSharedTG` / `isClusterTG` / `isPeerInterestedTG`).
The source trunk is excluded from the fanout.

This preserves the single-hop invariant:

- **Non-owners** receiving trunk audio deliver to local clients and stop —
  they never relay to another trunk.
- **The owner** is the one and only relay.  Because the source link is
  skipped, the fanout cannot echo back.

Example (refA prefix `240`, refB prefix `262`, refC prefix `222`, mesh
trunked, a client on each of refA and refC monitoring TG `2626` which is
owned by refB):

```
refA client PTT on 2626
  └─► refA forwards to refB (isSharedTG)
        refB broadcasts to local clients on 2626
        refB fans out to refC via refB↔refC link (isPeerInterestedTG)
          └─► refC broadcasts to local clients on 2626 (no further relay)
```

**Scales to any mesh size.**  In a mesh of N reflectors with owner B and
N−1 non-owners, when any non-owner talks:

- sender → B directly (prefix match)
- B fans out to the other N−2 peers (source link excluded)
- each non-owner delivers locally and stops

Complexity per audio frame on the owner is **O(N) TCP sends** — inherent to
any mesh fanout, not a new limitation.

**Peer-interest caveat.**  The owner's fanout filter checks
`isPeerInterestedTG` on each non-owner link.  `m_peer_interested_tgs` is
populated when a peer emits `TrunkTalkerStart` or `TrunkAudio` on the TG
(see [Peer Interest Tracking](#peer-interest-tracking) below), so a
non-owner's client must PTT on the TG at least once before the owner knows
to forward that TG back to them.  This is the same mechanism that enables
the return path in 2-reflector conversations, reused for the N−2 fanout.

The interest entry is **refreshed on every subsequent `TrunkTalkerStart`
/ `TrunkAudio` for the same TG** and **expires after 10 minutes of
inactivity** from that peer on that TG
(`PEER_INTEREST_TIMEOUT_S = 600` in `TrunkLink.h`), or immediately when
the trunk link fully disconnects.  In practice this means: during an
active QSO the fanout stays live indefinitely; on a TG that has been
silent for ~10 minutes, the first PTT from a non-owner's client re-arms
the fanout before the owner starts forwarding that TG back out.

### Cluster TGs

Cluster TGs bypass prefix routing entirely.  A TG listed in `CLUSTER_TGS` is
forwarded to **all** trunk peers regardless of `LOCAL_PREFIX` or `REMOTE_PREFIX`.
The routing decision in `TrunkLink` is:

```
Sending:   if isClusterTG(tg) or isSharedTG(tg) or isPeerInterestedTG(tg)  → send to peer
Receiving: if isClusterTG(tg) or isOwnedTG(tg)   → accept from peer
Otherwise: drop silently
```

This check is applied **independently on both sides** of each trunk link,
using different prefix checks appropriate to each direction:

- **Sending side:** `TrunkLink::onLocalTalkerStart`, `onLocalAudio`, and
  `onLocalFlush` check `isSharedTG(tg) || isClusterTG(tg) ||
  isPeerInterestedTG(tg)` before sending.  `isSharedTG` matches the TG
  against the **remote** peer's prefix — i.e. it forwards TGs that belong
  to the peer.  Additionally, `isPeerInterestedTG` matches TGs that the
  peer has previously sent traffic for, enabling the return path for
  bidirectional conversations (see below).
- **Receiving side:** `TrunkLink::handleMsgTrunkTalkerStart`,
  `handleMsgTrunkAudio`, and `handleMsgTrunkFlush` check
  `isOwnedTG(tg) || isClusterTG(tg)` before accepting.  `isOwnedTG` matches
  the TG against both the **local** prefix (TG belongs to us — a peer's client
  is talking on one of our TGs) and the **remote** prefix (TG belongs to the
  peer — the peer is reporting its own talker state).

Each reflector owner chooses which cluster TGs to subscribe to.  A reflector
only sends and accepts traffic for cluster TGs listed in its own `CLUSTER_TGS`.
If a cluster TG is declared on the sending reflector but not the receiving one,
the receiving side silently ignores the traffic — this is normal operation, not
a misconfiguration.  Only reflectors that both subscribe to a given cluster TG
will exchange audio for it.

### Peer Interest Tracking

Prefix-based routing is inherently one-directional: when a client on
reflector A visits a TG owned by reflector B, `isSharedTG` causes A to
forward to B.  But when B's client replies on that same TG, B sees it as a
local TG and has no reason to forward back to A.

To enable the return path, each `TrunkLink` tracks **peer interest**: when
a peer sends `MsgTrunkTalkerStart(tg)`, the receiving side records that the
peer has active clients on that TG.  From that point, local talker activity
on the same TG is also forwarded to the peer via `isPeerInterestedTG`.

Interest is refreshed on each `MsgTrunkAudio` received for the TG (keeping
it alive during long transmissions).  It expires after 10 minutes of
inactivity on that TG from the peer, and is cleared immediately on trunk
disconnect.  The interest map is per-link — disconnecting one peer does not
affect interest tracked for other peers.

### Satellite Links — No TG Filtering

Satellite links (`SatelliteClient` / `SatelliteLink`) do **not** apply any TG
filtering.  All talker signaling and audio is forwarded unconditionally in both
directions:

| Aspect | Trunk link | Satellite link |
|--------|-----------|----------------|
| Prefix filtering (`isSharedTG`/`isOwnedTG`) | Yes | No |
| Cluster filtering (`isClusterTG`) | Yes | No |
| What is forwarded | Only prefix-matched + cluster TGs | All TGs |
| `CLUSTER_TGS` config needed | Yes, on both sides | No |

---

## Connection and Handshake

Each reflector both listens for inbound trunk connections on its trunk server
port (default 5302) and actively connects outbound to each configured peer.
Outbound and inbound connections operate independently and can both be active
at the same time.  When sending, the outbound connection is preferred; if it is
not ready, the inbound connection is used as fallback.

### Outbound connection

When a `TrunkLink` connects outbound to a peer, it immediately sends
`MsgTrunkHello`.  The peer's trunk server accepts the connection, verifies the
HMAC-authenticated shared secret, matches it to the corresponding `TrunkLink`,
and sends its own `MsgTrunkHello` in response.

```
Reflector A (prefix "1")           Reflector B (prefix "2")
    │──── TCP connect to B:5302 ──────────────────────────►│
    │──── MsgTrunkHello(id, prefix="1", priority, HMAC) ──►│
    │                          B verifies HMAC, matches TrunkLink
    │◀─── MsgTrunkHello(id, prefix="2", priority, HMAC) ───│
    │              (both sides ready)                       │
```

### Inbound connection

The trunk server (`Reflector::m_trunk_srv`) listens on the configured
`TRUNK_LISTEN_PORT` (default 5302).  When a peer connects:

1. The connection enters a **pending** state with a 10-second hello timeout
2. The server waits for a `MsgTrunkHello` frame
3. The shared secret is verified via HMAC against each configured `TrunkLink`
4. On match, the connection is handed off to the corresponding `TrunkLink`
5. The `TrunkLink` stores the inbound connection alongside any existing
   outbound connection

**Security hardening:**
- Pre-authentication frame size is limited to 4 KB (vs 32 KB post-auth)
- Maximum 5 pending (unauthenticated) connections at a time
- 10-second timeout for hello message; connections that don't authenticate are
  dropped
- Peer identifiers are sanitized before logging (control characters stripped,
  length capped at 64)

### Dual Connection Model

Each `TrunkLink` maintains two independent connections to the peer: an outbound
connection (via `TcpPrioClient`) and an inbound connection (accepted by the
trunk server).  Both can be active simultaneously.  When sending messages, the
outbound connection is preferred; if it is not ready, the inbound connection is
used as fallback.  Each connection has independent heartbeat timers.

If a second inbound connection arrives while one is already active, the new one
is rejected.  Outbound reconnection is handled automatically by
`TcpPrioClient`.

### `MsgTrunkHello` fields

- `id` — the config section name (e.g. `TRUNK_1_2`), used for logging and
  connection matching — both sides must use the same section name
- `local_prefix` — the sender's authoritative TG prefix (e.g. `"1"`)
- `priority` — random 32-bit nonce, regenerated per connection, used for
  talker arbitration tie-breaking
- `nonce` + `digest` — HMAC authentication of the shared secret

---

## Normal Talker Flow

When a local client starts transmitting on a TG owned by the peer:

```
Reflector A (owns prefix "1")      Reflector B (owns prefix "2")
    │  SM0ABC starts on TG 25          │
    │  setTalkerForTG(25, SM0ABC)      │
    │──── MsgTrunkTalkerStart(25, "SM0ABC") ──►│
    │                     setTrunkTalkerForTG(25, "SM0ABC")
    │                     broadcast MsgTalkerStart(25, "SM0ABC") to B clients
    │──── MsgTrunkAudio(25, <frame>) ─►│
    │                     broadcast MsgUdpAudio to B TG25 clients
    │   (repeats for each audio frame) │
    │──── MsgTrunkFlush(25) ──────────►│
    │  setTalkerForTG(25, null)        │
    │──── MsgTrunkTalkerStop(25) ─────►│
    │                     clearTrunkTalkerForTG(25)
    │                     broadcast MsgTalkerStop(25, "SM0ABC") to B clients
    │                     broadcast MsgUdpFlushSamples to B TG25 clients
```

The reverse direction (B client talking on a TG owned by A) is symmetric.

---

## Talker Arbitration and Tie-Breaking

Each reflector arbitrates talkers independently for its own clients.  The trunk
adds a second layer of arbitration for shared TGs.

**Normal case (one side talking at a time):** the first `MsgTrunkTalkerStart`
to arrive locks the TG on the receiving side.  Local clients on that side cannot
grab the talker slot while the trunk lock is held — their audio packets are
silently dropped by the existing `talker != client` check in the reflector's
UDP handler.

**Simultaneous claim (both sides start at the same instant):** resolved using
the `priority` nonce exchanged in `MsgTrunkHello`:

- The side with the **lower** priority value wins
- The losing side calls `setTalkerForTG(tg, nullptr)` to clear its local talker,
  then calls `setTrunkTalkerForTG(tg, peer_callsign)` to accept the remote claim
- The losing side suppresses the spurious `MsgTrunkTalkerStop` that would
  otherwise be sent when the local talker is cleared (tracked via `m_yielded_tgs`)

---

## Local Client Arbitration Guard

In `Reflector::udpDatagramReceived()`, before accepting a new local talker on a
TG that is shared with any trunk, the reflector checks whether the trunk already
holds the TG:

```cpp
if ((talker == 0) && !TGHandler::instance()->hasTrunkTalker(tg))
{
    TGHandler::instance()->setTalkerForTG(tg, client);
    ...
}
```

This single guard ensures that remote conversations are protected from
interruption by local clients.

---

## Keepalive / Heartbeat

Both sides send `MsgTrunkHeartbeat` periodically to keep the TCP connection
alive and detect peer disconnection:

- TX: sent when no other message has been sent for 10 timer ticks (10 s)
- RX timeout: connection torn down if no message received for 15 ticks (15 s)

On disconnect, all trunk talker locks are immediately released so local clients
are not left blocked.  The `TcpPrioClient` infrastructure handles automatic
reconnection.

---

## Protocol Messages

All messages use the existing SvxReflector TCP framing: a serialized
`ReflectorMsg` header (containing the type field) followed by the message
payload, packed using the `ASYNC_MSG_MEMBERS` macro.

| Type | Name                  | Fields                                           |
|------|-----------------------|--------------------------------------------------|
| 115  | `MsgTrunkHello`       | `string id`, `string local_prefix`, `uint32 priority`, `uint8[] nonce`, `uint8[] hmac`, `uint8 role` |
| 116  | `MsgTrunkTalkerStart` | `uint32 tg`, `string callsign`                   |
| 117  | `MsgTrunkTalkerStop`  | `uint32 tg`                                      |
| 118  | `MsgTrunkAudio`       | `uint32 tg`, `uint8[] audio`                     |
| 119  | `MsgTrunkFlush`       | `uint32 tg`                                      |
| 120  | `MsgTrunkHeartbeat`   | *(no fields)*                                    |
| 121  | `MsgTrunkNodeList`    | `string[] callsigns`, `uint32[] tgs`, `float[] lats`, `float[] lons`, `string[] qth_names` |
| 122  | `MsgTrunkFilter`      | `string filter` (shared-syntax TG filter: exact / `24*` / `10-20`, comma-separated) |

Type numbers 115–120 are chosen to follow the last existing SvxReflector TCP
message type (114 = `MsgStartUdpEncryption`) without collision. Types 121–122
were added by the jayReflector integration (v1.1.0) and are optional —
older peers silently ignore unknown message types, preserving backward
compatibility.

`MsgTrunkNodeList` is emitted by the reflector on local client login, logout,
or TG change, debounced to 500 ms, and broadcast to every trunk peer. Peers
republish it via MQTT under `nodes/<peer_id>` so a central dashboard can see
who is connected to each reflector.

`MsgTrunkFilter` is used by satellite links to advertise TG interest to the
parent. A satellite with `SATELLITE_FILTER` set sends one `MsgTrunkFilter`
right after authenticating; the parent stores the filter and skips forwarding
non-matching TGs to that satellite. Absent or empty filter means pass-all
(pre-existing behavior). Trunk peers do not emit it.

Audio is transported over TCP (not UDP) — the trunk is a reliable server-to-server
link, not a lossy radio gateway path, so TCP framing is appropriate and avoids
the complexity of a separate UDP cipher channel.

---

## TGHandler Changes

Trunk talker state is stored separately from the `ReflectorClient*` talker
pointer in `TGHandler`, in a parallel `std::map<uint32_t, std::string>` keyed
by TG number.  This avoids any refactoring of the existing `ReflectorClient`
type hierarchy.

New methods:
- `setTrunkTalkerForTG(tg, callsign)` — lock TG, emit `trunkTalkerUpdated` signal
- `clearTrunkTalkerForTG(tg)` — release lock on one TG
- `clearAllTrunkTalkers()` — release all locks (used on disconnect)
- `trunkTalkerForTG(tg)` — returns callsign or `""` if none
- `hasTrunkTalker(tg)` — returns bool, used in arbitration guard
- `trunkTalkersSnapshot()` — returns const ref to the map (for HTTP status)

New signal:
- `trunkTalkerUpdated(tg, old_callsign, new_callsign)` — connected to
  `Reflector::onTrunkTalkerUpdated()` which broadcasts `MsgTalkerStart`/`Stop`
  to local clients with the correct remote callsign

---

## HTTP Status

The `/status` JSON endpoint includes a `trunks` object alongside `nodes`:

```json
{
  "nodes": { ... },
  "trunks": {
    "TRUNK_1_2": {
      "host": "reflector-b.example.com",
      "port": 5302,
      "connected": true,
      "local_prefix": "1",
      "remote_prefix": "2",
      "active_talkers": {
        "25": "SM0ABC",
        "200": "LA8PV"
      }
    }
  }
}
```

`active_talkers` contains only TGs with an active remote talker at the moment
of the request.  It is empty when no trunk conversations are in progress.

---

## Configuration

### Global setting (both reflectors)

Add `LOCAL_PREFIX` to the `[GLOBAL]` section.  This declares which TG prefix
this reflector is authoritative for.  `TRUNK_LISTEN_PORT` sets the TCP port
the trunk server listens on (default 5302):

```ini
[GLOBAL]
...
LOCAL_PREFIX=1            # this reflector owns TGs 1, 10, 100, 1000, ...
TRUNK_LISTEN_PORT=5302    # trunk server port (default 5302, optional)
```

### Per-trunk section

Add one `[TRUNK_x]` section per peer.  Both reflectors in a link **must use the
same section name** — the name is exchanged during the handshake and used to
match connections.  The convention is to use sorted pair identifiers (e.g.
`[TRUNK_1_2]` for the link between reflector 1 and reflector 2).
`REMOTE_PREFIX` declares which TG prefix the peer owns:

```ini
[TRUNK_1_2]
HOST=reflector-b.example.com   # peer hostname or IP
PORT=5302                       # peer trunk port (default 5302)
SECRET=change_this_secret       # shared secret
REMOTE_PREFIX=2                 # peer owns TGs 2, 20, 200, 2000, ...
```

### Full-mesh example (three reflectors)

**Reflector 1** (`LOCAL_PREFIX=1`):
```ini
[TRUNK_1_2]
HOST=reflector-b.example.com
SECRET=secret_ab
REMOTE_PREFIX=2

[TRUNK_1_3]
HOST=reflector-c.example.com
SECRET=secret_ac
REMOTE_PREFIX=3
```

**Reflector 2** (`LOCAL_PREFIX=2`):
```ini
[TRUNK_1_2]
HOST=reflector-a.example.com
SECRET=secret_ab
REMOTE_PREFIX=1

[TRUNK_2_3]
HOST=reflector-c.example.com
SECRET=secret_bc
REMOTE_PREFIX=3
```

**Reflector 3** (`LOCAL_PREFIX=3`):
```ini
[TRUNK_1_3]
HOST=reflector-a.example.com
SECRET=secret_ac
REMOTE_PREFIX=1

[TRUNK_2_3]
HOST=reflector-b.example.com
SECRET=secret_bc
REMOTE_PREFIX=2
```

---

## Files Changed vs Upstream SvxLink

| File | Change |
|------|--------|
| `src/svxlink/reflector/ReflectorMsg.h` | Added `MsgTrunkHello`–`MsgTrunkHeartbeat` (types 115–120); `MsgTrunkHello` carries `local_prefix` string instead of TG list |
| `src/svxlink/reflector/TGHandler.h` | Added trunk talker map, 5 methods + snapshot accessor, `trunkTalkerUpdated` signal |
| `src/svxlink/reflector/TGHandler.cpp` | Implemented trunk talker methods including `clearAllTrunkTalkers` |
| `src/svxlink/reflector/Reflector.h` | Added `m_trunk_links`, `m_trunk_srv`, `initTrunkLinks()`, `initTrunkServer()`, `onTrunkTalkerUpdated()`, trunk inbound connection handling |
| `src/svxlink/reflector/Reflector.cpp` | Wired trunk init, trunk server (listen + accept + auth), signals, audio relay, arbitration guard, HTTP status |
| `src/svxlink/reflector/TrunkLink.h` | **New** — `TrunkLink` class declaration |
| `src/svxlink/reflector/TrunkLink.cpp` | **New** — full trunk link implementation |
| `src/svxlink/reflector/SatelliteLink.h` | **New** — `SatelliteLink` class declaration |
| `src/svxlink/reflector/SatelliteLink.cpp` | **New** — satellite link implementation (parent side) |
| `src/svxlink/reflector/SatelliteClient.h` | **New** — `SatelliteClient` class declaration |
| `src/svxlink/reflector/SatelliteClient.cpp` | **New** — satellite client implementation (satellite side) |
| `src/svxlink/reflector/CMakeLists.txt` | Added `TrunkLink.cpp` to sources |
| `src/svxlink/reflector/svxreflector.conf.in` | Added prefix-based trunk example config |
| `src/versions` | Version bumped to `1.3.99.12+trunk1` |
| `src/svxlink/reflector/svxreflector.cpp` | Updated startup banner |
