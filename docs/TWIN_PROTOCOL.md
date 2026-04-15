# GeuReflector — Twin (HA-Pair) Protocol — DESIGN DRAFT

> **Status:** design draft, not yet implemented. Open questions are flagged
> in the final section. Reviewed by the Italian and German reflector sysops
> before any code is written.

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
- The twin link (`[TWIN]` section) mirrors local-client activity between
  the two nodes: talker-start/stop, audio frames, TG membership, and
  whatever subset of `MsgTrunkNodeList` / `MsgTrunkFilter` state is
  useful. This is semantically closer to `SatelliteLink` than to
  `TrunkLink`, but symmetric.
- Only **one twin at a time** initiates and serves the pair's external
  trunks (the *primary*). The other (the *standby*) keeps its
  `TrunkLink` instances dormant. On primary failure the standby
  promotes. See §External-Trunk Ownership below.
- Cross-twin talker arbitration reuses the existing priority-nonce
  tie-break: the same 32-bit `priority` field from `MsgTrunkHello`
  decides who wins when both twins' clients PTT the same TG at the same
  instant.

---

## Relationship to Existing Link Types

| Aspect                         | Trunk (`[TRUNK_x]`)         | Satellite (`[SATELLITE]`)        | **Twin (`[TWIN]`)**               |
|--------------------------------|-----------------------------|----------------------------------|-----------------------------------|
| Directionality                 | Symmetric                   | One-way (parent → satellite bias) | Symmetric                         |
| Prefix ownership               | Distinct per node           | N/A                              | **Identical** on both nodes       |
| TG filtering                   | `isSharedTG` / `isOwnedTG`  | None (all TGs forwarded)         | None (full mirror within the pair) |
| Number of peers per section    | 1                           | Many satellites per parent       | **Exactly 1** (strictly pairwise) |
| External trunk ownership       | Each node holds its own     | Parent-only                      | Primary holds; standby dormant    |
| Cross-node client arbitration  | Priority-nonce (inter-site) | Parent dictates                  | **Priority-nonce (intra-pair)**   |

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

| Type | Name                  | Purpose on twin link                              |
|------|-----------------------|---------------------------------------------------|
| 115  | `MsgTrunkHello`       | Handshake (role=ROLE_TWIN)                        |
| 116  | `MsgTrunkTalkerStart` | Mirror local talker start to the twin             |
| 117  | `MsgTrunkTalkerStop`  | Mirror local talker stop                          |
| 118  | `MsgTrunkAudio`       | Mirror local client audio                         |
| 119  | `MsgTrunkFlush`       | Mirror end-of-stream                              |
| 120  | `MsgTrunkHeartbeat`   | Keepalive (10s TX / 15s RX timeout, as today)     |
| 121  | `MsgTrunkNodeList`    | Mirror node roster so `/status` is consistent     |
| 122  | `MsgTrunkFilter`      | **Not used on twin links** — twins mirror every TG unconditionally, so there is no interest advertisement to send. Reserved for satellite use only. |
| 123  | `MsgTwinRoleClaim`    | **NEW** — primary/standby election for external trunks |
| 124  | `MsgTwinRoleState`    | **NEW** — current role announcement (primary / standby) |

Reserved types 123–124 follow the jayReflector-added 121–122 without
collision.

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

## External-Trunk Ownership (Primary / Standby)

**Recommendation for v1: active/standby.** Rationale: it sidesteps
deduplication on the external peer and keeps the existing `TrunkLink`
state machine unmodified.

### Election

At twin-link establishment, both sides exchange `MsgTwinRoleClaim`
carrying their `priority` nonce. Lower nonce becomes primary. The
winner announces `MsgTwinRoleState(role=PRIMARY)`, the loser responds
with `MsgTwinRoleState(role=STANDBY)`.

- Primary: starts its configured `[TRUNK_x]` sections normally.
- Standby: loads `[TRUNK_x]` config but does **not** start the
  `TrunkLink` outbound connectors and rejects any inbound trunk
  connections (reply with a log-friendly close).

### Failover

If the twin link dies (heartbeat RX timeout), the standby must decide:

- If an external trunk heartbeat is still healthy from the standby's
  side → do nothing; the standby stays standby and the primary keeps
  serving. (This is the split-brain safe case: standby sees the primary
  is still alive via an external path.)
- If the standby has **no evidence** the primary is still alive → wait
  `TWIN_FAILOVER_HOLDOFF` (default 5s), then promote: start its
  `TrunkLink` instances.

On twin-link reconnect, the two nodes compare `priority` nonces again.
If the original primary is lower, the promoted standby demotes
gracefully (closes its external trunks once the primary confirms via
`MsgTwinRoleState(PRIMARY)`).

### Split-brain policy

If the twin link dies but both nodes believe they should be primary,
the external peer may briefly see two trunks claiming the same section
name + secret. Mitigations:

1. The external peer's existing "only one inbound per section" rule
   (`TrunkLink::onInboundConnected`) will reject the second.
2. Both twins are configured with the same external secret and section
   name — whichever gets the outbound connection first holds it.
3. On twin-link recovery, the higher-priority nonce's owner wins and
   tears down its external trunks.

This is not perfect but it is bounded and self-healing. A stricter
alternative (quorum with a third arbiter) is listed under open
questions.

---

## Configuration

### On ref1 (German reflector A)

```ini
[GLOBAL]
LOCAL_PREFIX=262

[TWIN]
HOST=ref2.example.de
PORT=5304                          # new default port for twin, TBD
SECRET=shared_twin_secret

# External trunks are declared as usual; they are only
# activated on whichever twin wins primary election.
[TRUNK_IT_DE]
HOST=refa.example.it
PORT=5302
SECRET=shared_trunk_secret
REMOTE_PREFIX=222
```

### On ref2 (German reflector B)

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

### On refA (Italian reflector, unchanged)

```ini
[GLOBAL]
LOCAL_PREFIX=222

[TRUNK_IT_DE]
HOST=de-anycast.example.de         # DNS round-robin or VIP pointing to
                                   # whichever German twin is primary
PORT=5302
SECRET=shared_trunk_secret
REMOTE_PREFIX=262
```

Alternatively refA can list both hosts if we add a simple "try each
host in order" option to `TcpPrioClient` (probably already supported
via its existing priority-ordered host list — to be checked during
implementation).

### New port

Default `TWIN_LISTEN_PORT=5304`, configurable. Separate from the trunk
port so inbound validation logic stays cleanly separated.

---

## Implementation Sketch (non-binding)

- New `TwinLink.cpp` / `TwinLink.h`, structurally similar to
  `TrunkLink` but:
  - no prefix filtering (`isSharedTG`/`isOwnedTG` replaced by
    unconditional accept for the paired node),
  - `priority` drives primary/standby election, not per-TG arbitration
    (except for the simultaneous-PTT tiebreak, which stays),
  - owns a small state machine: `INIT → ELECTING → PRIMARY | STANDBY`
    with failover transitions.
- New twin TCP server in `Reflector`, analogous to the trunk server
  but listening on `TWIN_LISTEN_PORT`.
- `Reflector` gains a notion of `m_twin_role` that gates
  `initTrunkLinks()`: primary starts trunks, standby does not.
- `TGHandler` gets one small addition: a method to let a twin yield a
  local talker to its pair partner without also firing
  `MsgTrunkTalkerStop` to the external trunks (since external trunks
  are held by the primary only, this may be a no-op — confirm during
  coding).
- `MsgTwinRoleClaim` / `MsgTwinRoleState` added to `ReflectorMsg.h`
  (types 123, 124).
- Config template (`svxreflector.conf.in`) gets a commented-out
  `[TWIN]` example.
- Integration test: add a 4-node topology to `tests/topology.py`
  — refA (IT, prefix 222), ref1+ref2 (DE twin pair, prefix 262), plus
  a third external reflector — to exercise failover and split-brain.

---

## Open Design Questions

These need explicit decisions before coding starts.

1. **Active/standby vs active/active for external trunks.**
   Active/standby is simpler and recommended for v1. Active/active
   would need a `twin_group_id` field in `MsgTrunkHello` so refA
   knows "these two trunks are one logical peer — dedupe audio by
   sequence number / TG+timestamp". Worth it for faster failover
   (no 5s holdoff) but significantly more code and a protocol
   extension visible to unrelated reflectors. **Proposal:** defer to v2.

2. **Split-brain quorum.** Should we support an optional third-party
   witness (e.g. a simple `[TWIN_WITNESS]` endpoint the pair pings to
   break ties) for operators who cannot tolerate even brief dual-primary?
   **Proposal:** ship v1 without it, add later if anyone reports pain.

3. **Client UDP audio: TCP-relayed or UDP multicast?** The trunk
   already relays audio over TCP (see TRUNK_PROTOCOL.md line 347).
   Twins are typically in the same datacenter / LAN and could use
   multicast, but that's a deployment assumption we shouldn't bake in.
   **Proposal:** TCP-relay, same as trunk. Revisit if LAN-deployed
   twins report CPU pressure.

4. **What about cross-twin `MsgTrunkNodeList` replication?**
   Do clients on ref1 need to see ref2's node roster in
   `MsgNodeList`, or should each twin advertise only its own clients
   externally? **Proposal:** mirror the roster internally so `/status`
   is identical on both twins, but have only the primary advertise
   the union to external trunks. Needs confirmation that
   `MsgTrunkNodeList` debouncing won't thrash during failover.

5. **Failover holdoff value.** 5s is conservative. Too short risks
   flapping on brief network hiccups; too long means up to 5s of
   silence on external TGs during a real failure. **Proposal:** 5s
   default, configurable via `TWIN_FAILOVER_HOLDOFF` in the `[TWIN]`
   section.

6. **Interaction with Redis-backed config store.** The current repo
   has a Redis config-store branch merged. Twin role (primary/standby)
   is runtime state, not config, so it should live in-process. But
   shared config (trunk secrets, prefix) already flows through Redis
   — twins reading the same Redis key get consistent config for free.
   **Proposal:** no Redis changes needed beyond what already works.

7. **Naming.** "Twin" vs "pair" vs "mirror" vs "HA link". `[TWIN]`
   is short, clear in the config, and unambiguous when paired with
   the existing `[TRUNK_x]` / `[SATELLITE]` section naming.
   **Proposal:** keep `[TWIN]`.

---

## Not in Scope

- More than two nodes per twin group. The primary/standby election is
  pairwise; generalizing to N-way would require a different algorithm
  (Raft-lite or similar) and is explicitly not supported.
- Geographic distribution with asymmetric latency. Twins are expected
  to be in the same datacenter or with a low-latency link (<50ms).
  Longer links should use regular trunk instead.
- Client-facing failover protocol changes. SvxLink clients already
  handle `HOSTS=ref1,ref2` with reconnect-on-failure; the twin protocol
  does not modify the client-facing protocol at all.
