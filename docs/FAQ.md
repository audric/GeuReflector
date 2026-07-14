# FAQ

Answers to the questions SvxReflector sysops ask most often when they first look
at GeuReflector.

---

## Before you switch

### Can I use my existing svxreflector config?

Yes, unchanged. Every key this fork adds is optional, and with none of them set
GeuReflector behaves exactly like the SvxReflector you are running today: no
prefix, no trunks, no satellites, no mesh. `LOCAL_PREFIX` is only consulted when
you actually configure a trunk or a twin.

Trunking is opt-in, and it starts the day you add a `[TRUNK_x]` section — not the
day you swap the binary.

### Do I need to patch my SvxLink nodes?

No. The client protocol is untouched, and patching svxlink is something this
project will not do. Your nodes run stock upstream SvxLink and cannot tell they
are talking to a fork.

Everything the mesh adds lives in message types 115–129, which are exchanged
*between reflectors*. A node never sees them.

### Why should I care about joining the international mesh?

Because your users get the world without you giving up your reflector.

You keep your own server, your own numbering, your own users, and your own rules.
Nothing is centralised: there is no master server that can go down and take you
with it, and no authority that owns your talk groups. What a trunk adds is that a
ham on your reflector can reach a talk group in another country, and hams there
can reach yours, without either of you renumbering anything or reconfiguring a
single radio.

The alternative — staying an island — costs your users everything outside your
own coverage, and costs you nothing less than the ability to be reached.

### Do I need a static IP or open ports?

Not necessarily — it depends on the link you use.

- **Trunk between two reflectors:** only **one** of the two peers needs to accept
  inbound TCP on 5302. Each trunk runs an outbound and an inbound leg
  independently, and sending prefers the outbound leg and falls back to the
  inbound one. So a reflector behind NAT can dial out to a peer with an open
  port, and the trunk is fully functional in both directions over that one socket.
- **Satellite:** dials out to its parent and needs **no** inbound port at all.
  This is what satellite mode is for.

In both cases you still need your own client port 5300 (TCP+UDP) reachable by
your own SvxLink nodes. That requirement has not changed from SvxReflector.

### How much bandwidth does a trunk use?

An idle trunk is almost free: a heartbeat every 10 s (with a 15 s receive
timeout), a TG-interest refresh every 60 s, and a node-list message only when
your client roster actually changes.

Audio costs what the codec costs — a trunk relays the codec frames your clients
are already sending, so a single active talker on a trunk is roughly one node's
audio stream. Crucially, **audio only crosses a trunk when the peer is interested
in that talk group**, so a busy mesh does not mean a busy trunk — you carry the
TGs your users care about, not everyone else's.

### If my reflector goes down, do my users lose everything?

They lose *your* reflector, and nothing else loses anything. There is no central
server, so your outage is yours alone: the rest of the mesh keeps running, it
simply cannot reach the talk groups under your prefix until you are back. Your
nodes reconnect on their own when you return.

---

## Talk groups and routing

### Do I have to renumber my talk groups to join the mesh?

**No.** Your users keep dialling the numbers they have always dialled, and
nothing in anyone's radio changes.

The mesh does need a globally unique number to route on, but that number does not
have to be the one your users type. `LOCAL_ALIASES` gives your short TG a routable
identity under your own prefix, behind the scenes — see the next question.

### How do I make my local TG selectable from foreign reflectors?

`LOCAL_ALIASES`, in `[GLOBAL]`. It re-homes a short local TG under a number you
own, so the mesh can route it while your users keep the short one.

```ini
[GLOBAL]
LOCAL_PREFIX=122
LOCAL_ALIASES=8:1220008
```

Your local hams keep dialling **8**. On the trunk wire the reflector advertises
**1220008**. A ham on any other reflector in the mesh selects **1220008** and
lands in the same conversation — his audio arrives tagged 1220008 and is folded
back to **8** for your users.

Because the alias is a real, owned, longest-prefix-matched number, it routes over
**every hop** in the mesh like any other owned TG. Configure it once, on the
owner. No peer has to cooperate and no intermediate gateway needs to know.

Two rules the parser enforces, and will reject the entry (with an error at
startup) if you break:

- the **alias** side must fall under one of your `LOCAL_PREFIX` values;
- the **short** side must *not* be a number you own.

Optional direction suffix: `8:1220008/out` publishes outward only, `/in` accepts
inward only; omitted means two-way, which is what you almost always want.

### Which do I want — LOCAL_ALIASES or CLUSTER_TGS?

The two get confused constantly. Pick by *who should hear it*:

| You want | Use | Configured | Reaches |
|---|---|---|---|
| Anyone in the mesh | `LOCAL_ALIASES` | once, on the owner | every hop |
| Everyone, by prior agreement | `CLUSTER_TGS` | every participating reflector | one hop from the originator |

The trap: **`CLUSTER_TGS` is single-hop.** A cluster TG is broadcast from the
originator to its direct peers and is *not* relayed onward. In a mesh of three or
more reflectors it is **not** a substitute for an alias.

### How does longest-prefix routing work?

A talk group is just a number, and the reflector that owns it is the one whose
prefix matches the most leading digits. Matching is on the decimal digits of the
TG, not on numeric ranges.

Every reflector builds one table at startup from three sources: its own
`LOCAL_PREFIX`, the `REMOTE_PREFIX` of each trunk (what that peer owns), and any
`ROUTABLE_PREFIXES` (what is reachable *through* that peer). To route a TG it
finds the longest prefix in that table that matches, and that tells it whose TG
it is and which trunk to send it down.

Say the mesh has `262` (a country) and `26201` (a region inside that country,
delegated to its own reflector):

| TG | Matches | Longest wins | Owner |
|---|---|---|---|
| `2624711` | `262` | `262` | the country reflector |
| `2620123` | `262`, `26201` | `26201` | the regional reflector |

This is what makes delegation work: the regional reflector's longer prefix
carves its range out of the country's shorter one, with no special-casing and
no cooperation needed from anyone else. It is also why prefixes must be unique —
a longer prefix is a more specific claim, but two identical prefixes are two
reflectors claiming the same numbers, which the match cannot resolve.

A trunk may also declare `ROUTABLE_PREFIXES=*` — a default route, used only when
no real prefix matches. Any real prefix always beats it.

### What happens if two reflectors claim the same prefix?

Don't do it. **A prefix has exactly one owner in the mesh.** Two reflectors
claiming the same one is a configuration error, and the reflector will not detect
it or warn you — routing runs off the prefixes you configure, and it trusts you.

What *is* allowed is a **longer** prefix. Routing is longest-prefix-match, so a
more specific prefix nested inside a shorter one is unambiguous and intentional:
that is how a sub-range gets delegated to another reflector (`ROUTABLE_PREFIXES`).
What is never allowed is two reflectors claiming the *same* prefix.

Prefix assignment is a coordination problem between operators, and GeuReflector
deliberately does not try to route around a bad one. Get your prefix from the
country's MCC.

(The one intentional exception is a **twin pair**, which shares a single prefix
by design — but a twin pair is one logical reflector, not two.)

### Can I monitor a foreign TG?

Yes, and nobody has to key up first to make it work.

Monitoring counts as *interest*: your reflector advertises the TGs your clients
have selected **or are monitoring** to the peer that owns them, so the owner
starts forwarding audio to you because it knows someone is listening. This works
across multiple hops.

### What does "interested" mean when people talk about TGs?

It is how a reflector knows which talk groups are worth sending to a given peer
at all. Each reflector tells its peers which TGs its own clients have selected or
are monitoring, and audio for a TG is only pushed down a trunk where somebody is
actually listening. That is what keeps a busy mesh from becoming a busy trunk.

Interest is refreshed continuously (on every packet for that TG, plus a 60 s
heartbeat) and **expires after 10 minutes** of complete silence on that TG from
that peer. This is normal and self-healing — the next transmission re-establishes
it — but it explains the occasional "the first over after a quiet spell seemed to
take a moment."

---

## Topologies

### How is a trunk different from an SvxLink node connection?

**A node carries one conversation at a time. A trunk carries many, at once.**

An SvxLink node is a radio: one talk group, one station talking. A trunk is the
link between two reflectors, with a whole population of nodes behind it — several
QSOs on different TGs cross it simultaneously, each with its own talker.

The rest follows: a node logs in with a callsign and password on 5300 and sends
audio over UDP; a trunk authenticates with a shared secret on 5302 and carries
everything, audio included, over that one TCP connection — plus prefixes, TG
interest, and the peer's roster.

So you cannot join two reflectors by pointing one at the other as if it were a
node.

### How do I remove the single point of failure of an international mesh reflector?

Run a `[TWIN]` pair: two reflectors sharing one `LOCAL_PREFIX`.

Client failover is already yours and always was — stock SvxLink does it with
`HOSTS=ref1,ref2`, and needs nothing from this fork. What a twin pair adds is the
server-side half: the two reflectors stay in step with each other, and the rest
of the mesh still sees **one** peer. Without that, a node failing over lands on a
reflector the mesh treats as a different peer entirely.

It is real operational cost, so be sure you need it:

- **Every external reflector that trunks to you must use a single `[TRUNK_x]`
  section with `PAIRED=1` and both your hostnames.** Two separate sections cause
  duplicate audio and self-echo. This is a coordination burden you are placing on
  *other* operators.
- Both twins need inbound TCP 5304. Unlike satellites, **twins are not
  NAT-friendly**.
- The pair mirrors everything — no per-TG scoping between twins.
- If the twin link itself drops, both halves keep serving their own clients
  (split-brain is tolerated by design).

A twin pair is for **client HA on one site**. It is not a way to spread load —
that is what trunks are for.

---

## Running it

### What are the different trunk statuses?

A trunk is not one connection, it is **two independent legs** — an outbound one
you dial and an inbound one you accept — and either can carry traffic. Sending
prefers outbound and falls back to inbound. A trunk counts as *connected* when
**either** leg is up, which is why a healthy trunk can legitimately show one leg
down.

`TRUNK STATUS` on the command PTY reports one line per link:

```
TRUNK_AB peer_id=ab ob=up ib=down active_tgs=3 muted=0
```

`/status` exposes the same thing as booleans: `connected` (either leg ready),
plus `outbound_connected` / `outbound_hello` and `inbound_connected` /
`inbound_hello`. A leg is only *ready* once its handshake (`hello`) has completed
— TCP being connected is not enough.

### Do I have to expose my /status JSON to the world?

**No — and your peers never needed it.** This is the most common misconception
about `/status`.

Trunk peers exchange everything they need over the trunk TCP link itself: the
client roster arrives as a peer-protocol message, and satellites and twins get
richer per-client updates the same way. **Nothing in the mesh reads HTTP.** The
`/status` endpoint exists purely for *your own* dashboards and tooling.

So you can firewall it, bind it away, or simply not set `HTTP_SRV_PORT` at all,
and the mesh works exactly as before.

Be aware of what it is if you *do* enable it: `/status` is unauthenticated,
read-only, and listens on **all interfaces** — there is no bind-address or ACL
option today. If you enable it, put it behind your firewall or a reverse proxy.

### Should I enable MQTT?

It depends on your scale, and the honest answer for a small setup is *no, don't
bother*.

- **Standalone or a small mesh:** polling `/status` is fine. Leave `[MQTT]` out
  entirely; there is zero overhead when it is not configured.
- **Facing the international mesh with a live dashboard:** yes. Your roster
  becomes every node on every reflector, and a dashboard re-polling a full
  snapshot to notice that one node changed TG is doing an enormous amount of work
  to learn almost nothing. Full snapshots cost O(nodes) *per poll*; MQTT's
  per-event topics cost O(changes).

At a few hundred nodes with rich per-client status, the full snapshot is hundreds
of KB — that is the point where polling stops being reasonable.

### How reactive is the MQTT publishing?

Fast, and edge-triggered rather than polled — messages are published when
something changes, not on a timer.

- **Talker start/stop:** immediate, no debounce. Fast enough to drive a live
  talker display.
- **A local client's rx status:** immediate, at the native update rate.
- **Per-client updates arriving from a satellite or twin:** debounced 500 ms at
  the sender, so a chattering PTT cannot flood the broker.
- **The full status snapshot:** republished periodically (30 s by default). This
  is a bootstrap and drift-correction mechanism, not the live path — do not build
  a dashboard that waits for it.
