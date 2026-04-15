# GeuReflector — Twin (HA-Pair) Protocol

> **Status:** Implemented in v1.3.x+twin1. See §Implementation for
> what shipped and `tests/test_twin.py` for integration test coverage
> (8 tests including an end-to-end V2-client audio-mirror path).

## Motivation

Some operators want two reflector instances to act as a redundant pair so
that SvxLink nodes can list both in `HOSTS=ref1,ref2` and transparently
fail over. Today this can be faked by giving both nodes overlapping
`LOCAL_PREFIX` lists and connecting them with a regular trunk — but this
has three problems:

1. **Prefix ownership becomes ambiguous.** Both nodes claim the same
   prefixes, so the "one authoritative home reflector per TG" invariant
   that the trunk protocol relies on no longer holds.
2. **External trunks can't use the natural prefix.** If ref1 and ref2
   want the outside world to see them as prefix `262`, they cannot also
   claim `1..9` internally without breaking the `REMOTE_PREFIX` match on
   the inbound validator (`Reflector.cpp:2465-2508`) of any external peer
   configured with `REMOTE_PREFIX=262`.
3. **No cross-node talker arbitration.** A German on ref1 and a German
   on ref2 can both PTT the same TG simultaneously with no coordination,
   because each reflector only arbitrates its own clients.

The **twin link** is a new, purpose-built link type that solves all three
by making the pair a single logical reflector from the outside and from
the perspective of TG arbitration, while still allowing clients to
connect to either physical node.

---

## Model

```
   Clients (SvxLink nodes, HOSTS=ref1,ref2)
       │           │           │
       ▼           ▼           ▼
      ref1  ◄──── twin ────►  ref2
   LOCAL_PREFIX=262        LOCAL_PREFIX=262
       │                       │
       └──── external trunks ──┘
                  ▼
                 refA  (Italy, LOCAL_PREFIX=222,
                        REMOTE_PREFIX=262 on its [TRUNK_*] to the pair)
```

- Both twins declare **identical** `LOCAL_PREFIX` (e.g. `262`). This is
  now legitimate, not a hack.
- The twin link (`[TWIN]` section) mirrors the full `TGHandler` state
  between the two nodes — both local-client talker state *and*
  externally-received trunk-talker state — plus node-roster and
  audio frames. Semantically closer to `SatelliteLink` than to
  `TrunkLink`, but symmetric and carries more state.
- **Both twins hold external trunks simultaneously** (active/active).
  External peers (e.g. refA) see the pair as **one logical trunk with
  two TCP endpoints** and send each frame to one endpoint at a time
  (sticky selection), failing over to the other endpoint on socket
  failure. This gives zero-holdoff failover at the cost of external
  peers needing to understand "paired hosts" in their trunk config.
- Cross-twin talker arbitration reuses the existing priority-nonce
  tie-break: the same 32-bit `priority` field from `MsgTrunkHello`
  decides who wins when both twins' clients PTT the same TG at the same
  instant.
- During a twin-link outage both twins keep operating fully and
  independently. Duplicate-talker artifacts are tolerated for the
  duration of the outage (see §Split-Brain Behavior).

---

## Relationship to Existing Link Types

| Aspect                         | Trunk (`[TRUNK_x]`)         | Satellite (`[SATELLITE]`)        | **Twin (`[TWIN]`)**               |
|--------------------------------|-----------------------------|----------------------------------|-----------------------------------|
| Directionality                 | Symmetric                   | One-way (parent → satellite bias) | Symmetric                         |
| Prefix ownership               | Distinct per node           | N/A                              | **Identical** on both nodes       |
| TG filtering                   | `isSharedTG` / `isOwnedTG`  | None (all TGs forwarded)         | None (full mirror within the pair) |
| Number of peers per section    | 1                           | Many satellites per parent       | **Exactly 1** (strictly pairwise) |
| External trunk ownership       | Each node holds its own     | Parent-only                      | **Both hold; external peer picks one socket per frame** |
| Cross-node client arbitration  | Priority-nonce (inter-site) | Parent dictates                  | **Priority-nonce (intra-pair)**   |
| Heartbeat cadence              | 10s TX / 15s RX             | 10s TX / 15s RX                  | **2s TX / 5s RX** (LAN-close)     |

Key observation: a twin link is full-mirror (like a satellite) but
symmetric (like a trunk) and strictly 1:1 (unlike either).

---

## Protocol

### Handshake

Reuse `MsgTrunkHello` (type 115) with a new role value:

```cpp
static const uint8_t ROLE_PEER      = 0;   // existing
static const uint8_t ROLE_SATELLITE = 1;   // existing
static const uint8_t ROLE_TWIN      = 2;   // NEW
```

Both sides send `MsgTrunkHello` with `role=ROLE_TWIN`, the shared
`[TWIN]` section name as `id`, the common `LOCAL_PREFIX` as
`local_prefix`, an HMAC over `secret`, and a fresh 32-bit `priority`
nonce.

Validation on receipt:

- `role` must be `ROLE_TWIN` — rejecting a peer that tries to speak
  trunk or satellite on a twin link.
- `local_prefix` **must equal** the local reflector's own
  `LOCAL_PREFIX` (sorted-set equal). This is the opposite rule from
  trunk: there, the peer's prefix must match a *different* declared
  `REMOTE_PREFIX`; here, the peer's prefix must match *ours*, because
  twins share ownership.
- HMAC over `secret` must verify.

Mismatch on any of the three → reject with a clear log line.

### Steady-state messages

The twin link carries a superset of what the trunk carries, with no TG
filtering:

| Type | Name                       | Purpose on twin link                              |
|------|----------------------------|---------------------------------------------------|
| 115  | `MsgTrunkHello`            | Handshake (role=ROLE_TWIN)                        |
| 116  | `MsgTrunkTalkerStart`      | Mirror local-client talker start                  |
| 117  | `MsgTrunkTalkerStop`       | Mirror local-client talker stop                   |
| 118  | `MsgTrunkAudio`            | Mirror local-client audio                         |
| 119  | `MsgTrunkFlush`            | Mirror end-of-stream                              |
| 120  | `MsgTrunkHeartbeat`        | Keepalive (**2s TX / 5s RX** on twin links)       |
| 121  | `MsgTrunkNodeList`         | Mirror node roster so `/status` is consistent on both twins |
| 122  | `MsgTrunkFilter`           | **Not used on twin links** — twins mirror every TG unconditionally. Reserved for satellite use. |
| 123  | `MsgTwinExtTalkerStart`    | **NEW** — "my external trunk peer `<peer_id>` has claimed TG X with callsign Y"; receiver updates its `TGHandler` trunk-talker map so local clients on the partner twin cannot key up that TG |
| 124  | `MsgTwinExtTalkerStop`     | **NEW** — clears the corresponding external-talker state |

No role-election messages are needed: both twins are always active, so
there is no primary/standby state to negotiate.

`MsgTwinExtTalkerStart` / `MsgTwinExtTalkerStop` carry `uint32 tg`,
`string peer_id` (the originating external trunk section name) and, for
`Start`, `string callsign`. This lets each twin attribute external state
to the correct external peer on reconnect/cleanup.

### Cross-twin talker arbitration

When `handleMsgTrunkTalkerStart` fires on a twin link, the receiver
treats it like a trunk talker-start **with prefix filtering disabled**:
any TG is accepted, and `TGHandler::setTrunkTalkerForTG(tg, callsign)`
is called so local clients on the receiving twin cannot grab the talker
slot. Tie-break when both sides claim simultaneously reuses the existing
priority-nonce rule (lower wins, loser yields via the existing
`m_yielded_tgs` machinery).

### Audio replication

`MsgTrunkAudio` on a twin link is broadcast to local clients on the
matching TG via `Reflector::broadcastUdpMsg` — same as the trunk path,
but without the `isOwnedTG` gate. The twin does *not* re-forward the
audio to its own external trunks (that would duplicate — external
trunks are only held by the primary; see below).

---

## External-Trunk Ownership (Active/Active)

Both twins start their configured `[TRUNK_x]` sections at boot and keep
them running continuously. There is no election, no role, no failover
holdoff.

### External peer view

An external peer (e.g. refA) that wants to trunk with a twin pair
declares a **single** `[TRUNK_x]` section with **multiple candidate
hosts** — one per twin — and a `PAIRED=1` flag:

```ini
[TRUNK_IT_DE]
HOST=ref1.example.de,ref2.example.de
PORT=5302
SECRET=shared_trunk_secret
REMOTE_PREFIX=262
PAIRED=1
```

The external peer opens TCP connections to **both** hosts and maintains
them in parallel. This requires extending `TrunkLink` to manage a list
of TCP endpoints instead of a single outbound + inbound pair (see
§Implementation).

### Sticky per-frame routing

The external peer picks **one socket at a time** for a given transmission
and uses it for all frames of that transmission. The choice is sticky:
whichever socket is currently healthy keeps being used until it fails.
On failure, the peer immediately retries the next frame on the other
socket. There is no holdoff and no in-flight audio is lost beyond the
single TCP send buffer.

Inbound (audio from Germans to Italy): each twin independently forwards
its own local clients' audio to refA over its own socket. Twin-mirrored
audio is **not** re-forwarded externally (the originating twin already
sent it directly). Which twin a given transmission arrives on depends
only on which twin the local client is physically connected to.

### Cross-twin external-talker state propagation

When a twin receives `MsgTrunkTalkerStart(tg=262xx, callsign=IW0ABC)`
from refA on its sticky socket, it:

1. Updates its own `TGHandler` trunk-talker state as usual.
2. Emits `MsgTwinExtTalkerStart(tg=262xx, peer_id="TRUNK_IT_DE",
   callsign="IW0ABC")` over the twin link.

The partner twin, on receipt, updates its own `TGHandler` trunk-talker
state identically — so local clients on the partner twin are blocked
from keying up 262xx, just as they would be if they were on the
receiving twin. Loop suppression: a twin does not re-mirror external
state it learned *via* the twin link.

### refA-side socket selection policy

Initial socket choice: whichever TCP connection succeeds first.
Subsequent failures cause the peer to switch atomically on the next
outbound frame. When a previously-failed socket recovers, it joins the
pool as an alternate but does not preempt the current sticky choice
until the next failure — avoiding flap.

---

## Configuration

### On ref1 and ref2 (the German twin pair)

Both twins carry the identical `LOCAL_PREFIX`, a `[TWIN]` section
pointing at the partner, and their own copy of each external trunk
section.

**ref1:**

```ini
[GLOBAL]
LOCAL_PREFIX=262

[TWIN]
HOST=ref2.example.de
PORT=5304
SECRET=shared_twin_secret

[TRUNK_IT_DE]
HOST=refa.example.it
PORT=5302
SECRET=shared_trunk_secret
REMOTE_PREFIX=222
```

**ref2:**

```ini
[GLOBAL]
LOCAL_PREFIX=262

[TWIN]
HOST=ref1.example.de
PORT=5304
SECRET=shared_twin_secret

[TRUNK_IT_DE]
HOST=refa.example.it
PORT=5302
SECRET=shared_trunk_secret
REMOTE_PREFIX=222
```

Both twins connect to refA with the same section name and secret. refA
accepts both inbound connections as two endpoints of one paired trunk
(see §External-Trunk Ownership).

### On refA (Italian reflector)

refA declares the pair as a single trunk section with multiple candidate
hosts and `PAIRED=1`:

```ini
[GLOBAL]
LOCAL_PREFIX=222

[TRUNK_IT_DE]
HOST=ref1.example.de,ref2.example.de
PORT=5302
SECRET=shared_trunk_secret
REMOTE_PREFIX=262
PAIRED=1
```

With `PAIRED=1`, refA's `TrunkLink`:

- Opens outbound TCP connections to **every** host in the list.
- Accepts inbound TCP connections from **every** host, matching them to
  this section by the usual handshake. The existing "one inbound per
  section" rule is relaxed when `PAIRED=1`: up to `len(HOST)` inbounds
  are accepted.
- Picks one active socket for the sticky per-frame outbound selection;
  the others are live alternates.

Without `PAIRED=1` the legacy semantics (single host, outbound+inbound)
are preserved — existing deployments are unaffected.

### New port

Default `TWIN_LISTEN_PORT=5304`, configurable. Separate from the trunk
port so inbound validation logic stays cleanly separated.

---

## Implementation

Where the pieces live in the source tree:

- **`src/svxlink/reflector/TwinLink.cpp` / `.h`** — the twin link itself.
  Structurally similar to `TrunkLink` but with no prefix filtering
  (accept everything from the paired node), 2s TX / 5s RX heartbeat,
  and the `priority` nonce used only for the simultaneous-PTT
  tiebreak. Mirrors all `TGHandler` mutations to the partner,
  including external trunk-talker state via
  `MsgTwinExtTalkerStart/Stop`.
- **`Reflector`** runs a twin TCP server on `TWIN_LISTEN_PORT`
  (default 5304), analogous to the existing trunk server. Inbound
  hellos are validated (role, HMAC, `local_prefix`) before handoff
  to `TwinLink::acceptInboundConnection`.
- **`TrunkLink`** is extended for `PAIRED=1` on a normal `[TRUNK_x]`
  section: `HOST=h1,h2,...` parses into a list of endpoints; one
  `TcpPrioClient` is opened per endpoint; the "one inbound per
  section" rule is relaxed to `len(HOSTS)` inbounds; sending uses
  sticky selection (one socket per transmission, instant failover
  to the next live one on TCP-level failure).
- **`TGHandler`** gains `setTrunkTalkerForTGViaPeer(tg, callsign,
  peer_id)`, `clearTrunkTalkersForPeer(peer_id)` and
  `peerIdForTG(tg)` so external trunk-talker state mirrored across
  the twin link can be cleaned up per-peer on disconnect without
  clobbering other peers' claims.
- **`ReflectorMsg.h`** adds types 123 / 124 (`MsgTwinExtTalkerStart`
  / `Stop`) and a new `ROLE_TWIN = 2` value on `MsgTrunkHello`.
- **`svxreflector.conf.in`** includes commented-out `[TWIN]` and
  `PAIRED=1` examples.
- **`tests/test_twin.py`** covers handshake, PAIRED trunk wiring,
  no-auth-error startup, kill/restart recovery, PAIRED failover, and
  an end-to-end audio-mirror test that uses two V2 clients (one
  talking on ref1, one listening on ref2) to exercise the full
  UDP→TGHandler→TwinLink→UDP path.

---

## Split-Brain Behavior

During a twin-link outage both twins continue operating fully and
independently. No side stops serving clients or tears down external
trunks. The external peer (refA) still has both TCP sockets open and
keeps sending on its sticky choice.

Artifacts that can occur while the twin link is down:

- A German client on ref1 and a different German client on ref2 can
  both PTT TG 262xx at the same time. Each twin is unaware of the
  other's talker and both forward `MsgTrunkTalkerStart` to refA over
  their own external sockets. refA will receive conflicting
  talker-starts on its two endpoints for the same logical trunk.
  Resolution: refA's sticky routing means it accepts whichever it
  sees first and the other twin's forwarded audio lands on the
  non-sticky socket; the non-sticky-side audio is dropped by refA's
  "audio only from the peer that claimed the TG" check (same rule
  already used inside `handleMsgTrunkAudio` today).
- External-talker state set on one twin is not mirrored to the other,
  so the partner twin may accept a local-client PTT that would
  otherwise have been blocked. The resulting audio flows to refA and
  may compete with the existing conversation. Outcome depends on
  refA's sticky choice and the `handleMsgTrunkAudio` talker check.

These artifacts are bounded to the duration of the twin-link outage
and heal automatically on reconnection: once the twin link is back,
subsequent talker-starts, audio frames and external-trunk events
propagate over it as normal, so state converges as soon as there is
new activity on each affected TG. There is no bulk state-dump on
reconnect — in-progress transmissions at the moment of disconnect
may be truncated on the receiving twin, but nothing is left in a
permanently inconsistent state.

We explicitly accept this behavior rather than adding a quorum witness
or a degraded-mode lockdown: the twin link is expected to be LAN-close
and highly reliable, and brief split-brain events are preferable to
losing service during the outage.

---

## Design Rationale

Summary of the key design choices and why they were made:

| Topic | Decision | Rationale |
|-------|----------|-----------|
| External-trunk ownership | **Active/active**, sticky per-frame socket selection | Zero-holdoff failover without requiring a primary/standby state machine |
| External-peer config shape | **Single `[TRUNK_x]` section with multiple hosts** + `PAIRED=1` | The pair really is one logical peer from refA's view; cleanest config |
| Twin-link outage behavior | **Both twins keep operating fully** | Brief split-brain tolerated; simpler than degraded-mode logic |
| External-talker state sync | **Full `TGHandler` state mirrored** via `MsgTwinExtTalkerStart/Stop` | Unified view on both twins so local PTT is blocked when external peer holds a TG |
| Audio transport twin↔twin | **TCP relay**, same framing as trunk | Works anywhere, no new cipher/auth story, bandwidth fine for 1:1 |
| Heartbeat cadence on twin link | **2s TX / 5s RX** | Twins LAN-close, fast `/status` convergence |
| `MsgTrunkNodeList` | Each twin advertises **only its own local clients** externally; twin link mirrors the roster internally for consistent `/status` on both twins | Connection identity on the external peer provides origin; no additive schema change to `MsgTrunkNodeList` needed |
| Redis config store | **No changes** | Role is runtime state, not config; shared config already syncs via Redis |
| Naming | **`[TWIN]`** | Short, clear, orthogonal to `[TRUNK_x]` / `[SATELLITE]` |

### Deferred to v2 / future

- Third-party witness for quorum (no operator has asked for it yet).
- UDP multicast for audio between same-LAN twins (only revisit if TCP
  relay is measured to be a CPU bottleneck).
- More than two nodes per twin group (would need a different
  algorithm — Raft-lite or similar — and is explicitly out of scope).

---

## Not in Scope

- Geographic distribution with asymmetric latency. Twins are expected
  to be in the same datacenter or on a low-latency link (<50ms).
  Longer links should use regular trunk instead.
- Client-facing failover protocol changes. SvxLink clients already
  handle `HOSTS=ref1,ref2` with reconnect-on-failure; the twin protocol
  does not modify the client-facing protocol at all.
