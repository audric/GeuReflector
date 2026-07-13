# Concepts

This page explains *how GeuReflector thinks* — the handful of ideas that make
the rest of the documentation click. It sits between the
[README glossary](../README.md#glossary) (one-line definitions) and
[`PEER_PROTOCOL.md`](PEER_PROTOCOL.md) (the byte-level wire spec). Read it once
and the deployment guides will make sense.

No prior trunk knowledge is assumed, but you should know what a reflector and a
talk group are — see the [README intro](../README.md#new-to-svxlink-reflectors-start-here)
if not.

---

## 1. The mental model: talk groups are phone numbers

A talk group is identified by a number, and GeuReflector routes it the way a
phone network routes a call: **by its leading digits**.

Each reflector is configured with one or more `LOCAL_PREFIX` values. A reflector
*owns* every talk group whose number starts with one of its prefixes:

```
LOCAL_PREFIX=222  →  owns 222, 2220, 2225, 22299, 222100, …
LOCAL_PREFIX=262  →  owns 262, 2620, 26200, …
```

The owner is the single authoritative home for those TGs across the whole mesh.
Everyone else reaches them *through* the owner.

### Longest prefix wins

Prefixes may overlap on purpose. If both `222` and `222100` exist in the mesh,
TG `222100` belongs to whoever owns `222100` — the **longest matching prefix
wins**. This is what lets a national reflector own `222` while delegating a
specific sub-range like `222100` to a regional or club reflector. (See
[routable prefixes](../README.md#5-routable-prefixes--hierarchical-and-non-full-mesh-topologies-optional)
for declaring such delegated routes.)

---

## 2. What happens when a client keys up

A client (an SvxLink node) selects a TG and presses PTT. Three cases:

- **The TG is owned locally.** The reflector behaves exactly like stock
  SvxReflector: it relays the audio to every other local client on that TG. If
  any *remote* reflector has shown interest in the TG, it is also sent over the
  trunk(s) to them.
- **The TG is owned by a peer.** The reflector forwards the audio over the trunk
  to the owning peer (selected by longest-prefix-match). The owner then relays
  it — see below.
- **The TG matches no prefix in the mesh.** It has no owner, so no trunk
  claims it (`isSharedTG` finds no link to forward on). It still relays
  locally among that reflector's own clients, exactly like stock SvxReflector
  — it is simply a **local-only** talk group. The one exception is a link
  configured with `ROUTABLE_PREFIXES=*`: that wildcard is a default route, so
  an unowned TG *is* forwarded up it. The peer then discards it (the TG
  matches neither its `LOCAL_PREFIX` nor the link's `REMOTE_PREFIX`), so
  nobody else hears it — but it does cross the wire. If a TG must never leave
  the reflector, prefer an explicit `ROUTABLE_PREFIXES` list over `*`.

The client never knows or cares which reflector owns the TG. That is the whole
point: nodes stay unmodified.

### Local numbering is opt-in — but the number decides routing

A reflector does **not** impose the prefix convention on its nodes; a node may
pick any TG number. What matters is that routing is decided **solely by
longest-prefix-match on the number, regardless of its length**:

- Conforming numbers are routable — wherever their prefix points, provided that
  prefix is owned by a reflector in the mesh. This is length-independent: if
  Italy's `222` is owned, `22210` routes to its owner just as surely as `222`
  itself does; likewise `26201` routes to the owner of Germany's `262`.
- A number that matches no mesh prefix has no owner and stays **local-only**
  (e.g. a short `1`–`199` TG in a typical MCC scheme, where countries live in
  `200`–`799` — it stays local only because nothing owns that prefix).

So: **obey the convention to be reachable; use a non-matching number to stay
local.** This lets a reflector keep private local TGs alongside its routable,
mesh-facing ones — with two caveats. A number you *intended* as local but that
happens to match a configured prefix **will** be sent to that owner: the number
is the routing key, and there is no separate "make this local" switch. And on a
link with `ROUTABLE_PREFIXES=*`, even a non-matching number is forwarded up
that default route (the peer discards it, so nobody hears it — but it leaves
the reflector).

---

## 3. Owner-relay: why the owner is the hub

In a full mesh every reflector has a direct trunk to every other. You might
expect the speaker's reflector to blast audio to all peers directly — but that
would duplicate audio and risk loops. Instead:

> **Only the TG's owner fans audio out to the rest of the mesh.**

A non-owner sends to the owner; the owner relays to every *other* peer that has
expressed interest (never back to the source). This "owner-relay" rule keeps a
single, predictable distribution point per TG and is what makes three-way and
larger conversations work without echo. It is a deliberate single-hop fanout —
the owner does not expect peers to re-relay among themselves.

---

## 4. Peer interest: how the owner knows who to send to

The owner only forwards a TG to peers that actually care about it. A peer
becomes "interested" in a TG in two ways:

- **It advertises interest** when one of its local clients selects or monitors
  that TG (via a `MsgPeerTgInterest` message). This works even before anyone
  speaks, so a listener hears the *first* transmission.
- **It speaks on the TG**, which implicitly marks interest.

Interest is **per-link** and **expires after 10 minutes of inactivity** on that
TG from that peer (or immediately when the trunk disconnects). This timeout is
why a TG that has been silent for a long time may take one transmission to
"warm up" again on a listener that never advertised explicit interest.

---

## 5. Cluster TGs: the nationwide broadcast exception

Some TGs should reach everyone regardless of prefix ownership — think
BrandMeister's nationwide talk groups. Those are **cluster TGs**, listed in
`CLUSTER_TGS`:

- A reflector both **sends** and **accepts** a cluster TG only if that TG is in
  its own `CLUSTER_TGS`. Two reflectors exchange a cluster TG only if *both*
  list it.
- Cluster TGs are broadcast from a local client out to every trunk peer.
- On the receive side they are deliberately **single-hop**: a cluster TG arriving
  over a trunk is played locally but *not* re-forwarded to other trunks. In a
  mesh with cycles, re-forwarding would loop — so the only way a cluster TG
  crosses the mesh is the originator's own fanout to each peer.

Cluster TG numbers must not overlap any `LOCAL_PREFIX` or `REMOTE_PREFIX`.

---

## 6. Talker arbitration: who wins a collision

What if two reflectors' clients key the same TG at the same instant? Each
reflector arbitrates its own local clients normally (first talker wins locally).
Across the trunk, ties are broken by a **random 32-bit priority nonce** that the
two reflectors exchange during their handshake (`MsgPeerHello`):

> **The lower nonce wins.** The loser defers, treats itself as receiving from
> the remote talker, and blocks its own local clients from interrupting until the
> winner stops.

This guarantees both sides converge on the same winner without a central
coordinator.

---

## 7. Beyond the full mesh (pointers)

The four ideas above cover a standard full mesh. Three extensions handle shapes a
flat mesh can't:

- **Gateway prefix routing / routable prefixes** — a reflector with two trunk
  legs (e.g. a country gateway between an international and a national mesh) can
  transit traffic toward an owner it doesn't itself own, by declaring
  `ROUTABLE_PREFIXES`. See the
  [README routable-prefixes section](../README.md#5-routable-prefixes--hierarchical-and-non-full-mesh-topologies-optional).
- **Satellites** — a lightweight reflector that connects *out* to a parent
  instead of joining the mesh (NAT-friendly); by default it mirrors all TGs in
  both directions. See [`DESIGN_SATELLITE_AND_CLUSTER.md`](DESIGN_SATELLITE_AND_CLUSTER.md).
- **Twins** — two reflectors sharing one prefix as a high-availability pair,
  appearing as a single peer to the rest of the mesh. See
  [`TWIN_PROTOCOL.md`](TWIN_PROTOCOL.md).

---

## 8. A worked example: a three-way QSO

Three reflectors, full mesh. **A** owns prefix `1`, **B** owns `2`, **C** owns `3`.
A listener on each is monitoring TG `25` (owned by **B**).

1. A client on **A** keys TG `25`. `25` is not A's; longest-prefix-match says
   **B** owns it. A sends the audio over the A–B trunk.
2. **B** is the owner. It plays `25` to its own local clients, and fans the audio
   out to every *other* interested peer — here, **C** (which advertised interest
   because its listener is monitoring `25`). B does **not** send back to A.
3. **C** receives `25` over the B–C trunk and plays it to its local listener.
4. Everyone on `25` — across all three reflectors — hears the transmission, with
   B as the single relay point. When the talker stops, the next person to key
   `25` (on any reflector) goes through the same path; trunk-level arbitration
   settles any simultaneous starts.

---

## 9. Local aliases: giving a short TG a global number

Section 2 established the rule: a number is only reachable mesh-wide if its
prefix is owned by a reflector. That leaves a common problem — a club or
region wants a short, easy-to-remember local TG (e.g. `8`) that its own users
already dial, but also wants it reachable from anywhere in the mesh by a
proper globally-unique number under the reflector's own prefix (e.g.
`2620008` under `LOCAL_PREFIX=262`). Renumbering the local TG isn't
attractive, and neither is asking every peer to special-case it.

`LOCAL_ALIASES` re-homes the short TG under the long, owned number: the
reflector treats `8` and `2620008` as the same conversation, uses the long
number for everything that crosses the mesh (routing, owner-relay), and the
short number for everything local clients see (selection, talker
arbitration). Local users keep dialing `8`; anyone elsewhere in the mesh
reaches the same traffic by dialing `2620008`.

This sits alongside two other tools that shape TG reachability, and it's easy
to reach for the wrong one:

- **`CLUSTER_TGS`** (section 5) is for TGs that **every** participating
  reflector already agrees to broadcast everywhere, regardless of ownership —
  a nationwide or mesh-wide talk group both sides opted into.
- **`TG_MAP`** (per `[TRUNK_x]` section) is a **per-link** curated remap: it
  translates one TG number to another on a single trunk, so it must be
  repeated identically on every link that needs it, and it is invisible to
  hops beyond that one link.
- **Local alias** re-homes a curated subset of your own local TGs under a
  number you already own, once, in `[GLOBAL]`. Because the long number is a
  real, longest-prefix-matched, owned number, it routes across every hop in
  the mesh exactly like any other owned TG — no per-link repetition, and no
  cooperation required from peers or intermediate gateways.

The underlying rule doesn't change: anything meant to be reachable
internationally must have one unique number that falls under a country (MCC)
prefix. A local alias doesn't create an exception to that — it's a way to let
a short, locally-convenient number satisfy it without forcing local users to
dial the long form.

---

## Where to go next

- [`PEER_PROTOCOL.md`](PEER_PROTOCOL.md) — the exact messages, handshake, and
  field layouts behind everything above.
- [`TOPOLOGY_EXAMPLES.md`](TOPOLOGY_EXAMPLES.md) — visual cookbook of the
  supported mesh shapes, with a "which topology do I want?" guide.
- [`TROUBLESHOOTING.md`](TROUBLESHOOTING.md) — when a trunk won't connect or
  audio won't cross.
