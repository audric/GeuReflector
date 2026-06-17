# Troubleshooting

Common failure modes when bringing up trunks, satellites, and twins, with what
to check first. If a concept here is unfamiliar, read [`CONCEPTS.md`](CONCEPTS.md).

**Two tools answer most questions:**

- **`GET /status`** (enable with `HTTP_SRV_PORT=8080`) — shows each trunk's
  `connected` state, active talkers, satellites, and the rosters received from
  peers. This is the fastest way to see what the reflector *thinks* is happening.
- **Logs** — for connection and routing detail. Use the leveled logger
  (see [`LOGGING.md`](LOGGING.md)); `TRUNK_DEBUG=1` exposes per-section HMAC and
  prefix-match decisions but is extremely verbose — **enable it only while
  diagnosing, then turn it off** (it logs every audio frame).

---

## A trunk won't connect

`/status` shows the trunk with `"connected": false`, or the log never reports a
hello exchange. Check, in order:

1. **Section names must be identical on both ends.** The `[TRUNK_x]` section
   name is part of the handshake. If reflector A uses `[TRUNK_AB]` and B uses
   `[TRUNK_BA]`, B rejects the connection. Both sysops must agree on one name.
2. **The `SECRET` must match** on both sides exactly. A mismatch fails the HMAC
   check; with `TRUNK_DEBUG=1` you will see the section matched on name but
   failed on secret.
3. **Port 5302/TCP must be reachable inbound** on *both* reflectors. Trunks are
   mutually dialed: each side connects outbound *and* listens inbound. A firewall
   or NAT blocking 5302 on either end prevents at least one direction. (If only
   one direction is blocked the link may still show connected via the other leg —
   but fix both.)
4. **Each reflector needs a stable address.** Use a static IP or stable DNS name
   in `HOST=`. A peer behind a changing dynamic IP cannot be reliably dialed —
   that is what [satellites](#a-satellite-cant-connect) are for.
5. **`REMOTE_PREFIX` should match the peer's `LOCAL_PREFIX`.** A wrong prefix
   does not stop the *connection*, but it breaks *routing* (next section).

> Note: trunks connect *peer to peer*, independent of any client login. You do
> not need `[USERS]`/`[PASSWORDS]` entries for a trunk — those are for nodes.

---

## The trunk is connected but audio doesn't cross

The link shows `connected: true`, but a client on one reflector can't hear a
client on another.

1. **Is the TG owned by the right reflector?** Audio for TG `25` only flows if
   some reflector's `LOCAL_PREFIX` matches `25` (here, prefix `2`). If nobody
   owns the prefix, there is no home to route to. Confirm with the owner's
   `local_prefix` in `/status`.
2. **For a nationwide TG, is it a cluster TG on *both* sides?** A cluster TG is
   exchanged only between reflectors that *both* list it in `CLUSTER_TGS`. If A
   lists `222` and B does not, B silently ignores it — by design.
3. **Give it one transmission to warm up.** A listener that never *selected* the
   TG relies on the owner having current "interest" for its link. Interest
   expires after 10 minutes of silence on that TG, so the first key-up after a
   long idle may be what re-establishes the path. A client that actually selects
   or monitors the TG advertises interest immediately and should not need this.
4. **Check per-trunk filters.** `BLACKLIST_TGS` is evaluated first and drops the
   TG outright; `ALLOW_TGS`, if set, drops everything not listed. `TG_MAP` may be
   rewriting the number. These are per-`[TRUNK_x]` and reloadable via the PTY
   (`TRUNK RELOAD <section>`); inspect them with `TRUNK STATUS <section>`.
5. **Is a callsign muted?** `TRUNK MUTE <section> <callsign>` suppresses that
   callsign's audio on the inbound path. Check `muted` in `/status` and
   `TRUNK UNMUTE` if needed.

---

## Audio crosses in only one direction

Usually an asymmetry between the two ends:

- **Filters differ.** A `BLACKLIST_TGS`/`ALLOW_TGS`/`TG_MAP` set on one side but
  not the other makes audio pass one way and not the other. Compare both
  sections.
- **Interest is one-sided.** If only one side has a client selecting the TG, only
  that side has advertised interest. The other direction starts flowing once a
  client there selects/keys the TG.
- **Multi-hop transit:** each intermediate hop must declare the same
  `ROUTABLE_PREFIXES` for the forward path; the return path relies on interest
  (and, for a single-uplink leaf, the `*` wildcard). Re-check every hop in the
  chain — there is no automatic propagation.

---

## A satellite can't connect

1. **Parent `[SATELLITE]` section present** with `LISTEN_PORT=5303` and a
   `SECRET=` (or a per-id `SECRET_<id>=` matching the satellite's
   `SATELLITE_ID`). A per-id mismatch rejects with **no fallback**.
2. **Satellite side** sets `SATELLITE_OF`, `SATELLITE_PORT`, `SATELLITE_SECRET`,
   and `SATELLITE_ID` in `[GLOBAL]` — and does **not** set `LOCAL_PREFIX`,
   `REMOTE_PREFIX`, `CLUSTER_TGS`, or any `[TRUNK_x]` section.
3. **Only the parent needs an open inbound port** (5303). The satellite dials
   out, so it can sit behind NAT.
4. **Seeing fewer TGs than expected?** A `SATELLITE_FILTER` on the satellite
   narrows scope in *both* directions. Empty/absent = all TGs. The active filter
   appears under `/status.satellites[<id>].filter`.

---

## Twin pair: duplicates or self-echo

If an external reflector trunking to a twin pair hears doubled audio or its own
transmissions back:

- The external peer **must use a single `[TRUNK_x]` section with `PAIRED=1`**,
  not two separate sections (one per twin). Two sections defeat the sticky
  per-transmission socket selection and cause duplication and self-echo. See
  [`TWIN_PROTOCOL.md`](TWIN_PROTOCOL.md) ("External peer view").

---

## A trunk keeps dropping / reconnecting

The link flaps; logs show repeated RX timeouts.

- Heartbeats run every ~10 s with a ~15 s receive timeout. A link that drops
  about every 15 s of quiet usually means heartbeats aren't arriving — check for
  packet loss, a stateful firewall/NAT idle-timeout killing the TCP session, or
  one side pausing (an overloaded or swapping host).
- Confirm both directions: `/status` exposes the outbound and inbound legs
  separately for twins; for trunks, a flapping link that still shows `connected`
  is usually surviving on one leg while the other fails — fix the failing
  direction's reachability.

---

## Still stuck?

Capture `/status` from both ends and a short log slice with the relevant
subsystem at debug level (see [`LOGGING.md`](LOGGING.md)). The combination of
"what each side thinks the link state is" plus "which sections matched on
name/secret/prefix" pinpoints the vast majority of trunk problems.
