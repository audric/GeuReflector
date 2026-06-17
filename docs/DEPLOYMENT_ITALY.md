# Italy Deployment Example

This document describes a full national deployment for Italy using one GeuReflector
instance per region, trunked together in a full-mesh topology.

## Why the trunk is necessary

A standard SvxReflector instance is a single centralized server. Without trunking,
a national deployment has only two options:

**Option A — one national reflector:**
all 20 regions connect their nodes to a single server. This works, but only
**one talk group can be active at a time across the entire country**. While Lazio
is in QSO on the national TG, no other region can have a simultaneous independent
conversation. Every transmission from every region travels through the same server,
and a single server outage takes the whole network down.

**Option B — independent regional reflectors bridged by SvxLink instances:**
it is possible to link two reflectors together by running a SvxLink instance that
connects to both as a client node, acting as an audio bridge. However, each such
bridge instance can only handle one TG at a time, and one instance is needed per
pair of reflectors. For 20 regions in full mesh that is 190 SvxLink bridge
instances just to cover one shared TG — and multiplied again for every additional
TG that needs to be shared. The operational complexity, resource usage, and
failure surface of managing hundreds of bridge processes makes this approach
impractical at national scale.

**With GeuReflector trunks:**
each region runs its own independent reflector (resilience, local autonomy), and
the trunk links connect them so they can share TGs selectively. All 20 regional
TGs can carry simultaneous independent QSOs, while a dedicated national TG is
available to all. The trunk handles talker arbitration automatically when two
regions try to use a shared TG at the same time.

| | Single reflector | Independent reflectors | GeuReflector trunk mesh |
|---|---|---|---|
| Simultaneous regional QSOs | No — one TG at a time | Yes, but one bridge per TG per pair | Yes |
| Inter-region communication | Yes | Yes, but 190+ bridge processes | Yes |
| Regional autonomy | No | Yes | Yes |
| Single point of failure | Yes | No | No |

---

## TG numbering

The TG numbers used here are **inspired by the Italian DMR TG numbering scheme**,
which is already well thought out, widely adopted, and recognised across the
amateur radio community. Reusing this structure avoids reinventing a numbering
plan and makes the allocation immediately familiar to Italian operators.

**This SvxLink/GeuReflector system is entirely independent of DMR and is not
connected to any DMR network.** The TG numbers are used here purely as a
convenient and meaningful identifier space — no DMR radio, hotspot, or repeater
is involved.

The Italian DMR TG numbers all start with the national prefix `222`. **Keep that
prefix in `LOCAL_PREFIX`** so the reflector advertises its membership in the
Italy MCC space (e.g. TG `22201` → `LOCAL_PREFIX=22201`). This avoids
collisions with other countries' deployments — every country's reflectors
should anchor their prefixes on their own MCC.

Both `LOCAL_PREFIX` and `REMOTE_PREFIX` accept a comma-separated list of prefixes,
so a single reflector instance can own multiple regions
(e.g. `LOCAL_PREFIX=22211,22212,22213` for Liguria, Piemonte and Valle d'Aosta).

---

## Reflector inventory

| Region                    | TG      | LOCAL_PREFIX | Suggested hostname                  |
|---------------------------|---------|--------------|-------------------------------------|
| LAZIO                     | 22201   | 22201        | svxref-lazio.example.it             |
| SARDEGNA                  | 22202   | 22202        | svxref-sardegna.example.it          |
| UMBRIA                    | 22203   | 22203        | svxref-umbria.example.it            |
| LIGURIA                   | 22211   | 22211        | svxref-liguria.example.it           |
| PIEMONTE                  | 22212   | 22212        | svxref-piemonte.example.it          |
| VALLE D'AOSTA             | 22213   | 22213        | svxref-valledaosta.example.it       |
| LOMBARDIA                 | 22221   | 22221        | svxref-lombardia.example.it         |
| FRIULI VENEZIA GIULIA     | 22231   | 22231        | svxref-friuli.example.it            |
| TRENTINO ALTO ADIGE       | 22232   | 22232        | svxref-trentino.example.it          |
| VENETO                    | 22233   | 22233        | svxref-veneto.example.it            |
| EMILIA ROMAGNA            | 22241   | 22241        | svxref-emilia.example.it            |
| TOSCANA                   | 22251   | 22251        | svxref-toscana.example.it           |
| ABRUZZO                   | 22261   | 22261        | svxref-abruzzo.example.it           |
| MARCHE                    | 22262   | 22262        | svxref-marche.example.it            |
| PUGLIA                    | 22271   | 22271        | svxref-puglia.example.it            |
| BASILICATA                | 22281   | 22281        | svxref-basilicata.example.it        |
| CALABRIA                  | 22282   | 22282        | svxref-calabria.example.it          |
| CAMPANIA                  | 22283   | 22283        | svxref-campania.example.it          |
| MOLISE                    | 22284   | 22284        | svxref-molise.example.it            |
| SICILIA                   | 22291   | 22291        | svxref-sicilia.example.it           |

---

## Topology

20 reflectors × 19 trunk links each = **190 trunk TCP connections** total (full mesh).
Every pair shares a unique `SECRET`. Port `5302` is used for all trunk connections;
port `5300` remains for SvxLink client nodes.

---

## Shared secrets convention

Each pair of reflectors shares one secret. Use a naming convention to keep them
organised, for example `IT_<PREFIX_A>_<PREFIX_B>` (lower prefix first):

```
IT_01_02   # LAZIO ↔ SARDEGNA
IT_01_03   # LAZIO ↔ UMBRIA
…
IT_84_91   # MOLISE ↔ SICILIA
```

---

## Configuration: LAZIO (complete example)

```ini
[GLOBAL]
LISTEN_PORT=5300
LOCAL_PREFIX=22201
CLUSTER_TGS=222
HTTP_SRV_PORT=8080
COMMAND_PTY=/dev/shm/reflector_ctrl

[TRUNK_01_02]
HOST=svxref-sardegna.example.it
PORT=5302
SECRET=IT_01_02
REMOTE_PREFIX=22202

[TRUNK_01_03]
HOST=svxref-umbria.example.it
PORT=5302
SECRET=IT_01_03
REMOTE_PREFIX=22203

[TRUNK_01_11]
HOST=svxref-liguria.example.it
PORT=5302
SECRET=IT_01_11
REMOTE_PREFIX=22211

[TRUNK_01_12]
HOST=svxref-piemonte.example.it
PORT=5302
SECRET=IT_01_12
REMOTE_PREFIX=22212

[TRUNK_01_13]
HOST=svxref-valledaosta.example.it
PORT=5302
SECRET=IT_01_13
REMOTE_PREFIX=22213

[TRUNK_01_21]
HOST=svxref-lombardia.example.it
PORT=5302
SECRET=IT_01_21
REMOTE_PREFIX=22221

[TRUNK_01_31]
HOST=svxref-friuli.example.it
PORT=5302
SECRET=IT_01_31
REMOTE_PREFIX=22231

[TRUNK_01_32]
HOST=svxref-trentino.example.it
PORT=5302
SECRET=IT_01_32
REMOTE_PREFIX=22232

[TRUNK_01_33]
HOST=svxref-veneto.example.it
PORT=5302
SECRET=IT_01_33
REMOTE_PREFIX=22233

[TRUNK_01_41]
HOST=svxref-emilia.example.it
PORT=5302
SECRET=IT_01_41
REMOTE_PREFIX=22241

[TRUNK_01_51]
HOST=svxref-toscana.example.it
PORT=5302
SECRET=IT_01_51
REMOTE_PREFIX=22251

[TRUNK_01_61]
HOST=svxref-abruzzo.example.it
PORT=5302
SECRET=IT_01_61
REMOTE_PREFIX=22261

[TRUNK_01_62]
HOST=svxref-marche.example.it
PORT=5302
SECRET=IT_01_62
REMOTE_PREFIX=22262

[TRUNK_01_71]
HOST=svxref-puglia.example.it
PORT=5302
SECRET=IT_01_71
REMOTE_PREFIX=22271

[TRUNK_01_81]
HOST=svxref-basilicata.example.it
PORT=5302
SECRET=IT_01_81
REMOTE_PREFIX=22281

[TRUNK_01_82]
HOST=svxref-calabria.example.it
PORT=5302
SECRET=IT_01_82
REMOTE_PREFIX=22282

[TRUNK_01_83]
HOST=svxref-campania.example.it
PORT=5302
SECRET=IT_01_83
REMOTE_PREFIX=22283

[TRUNK_01_84]
HOST=svxref-molise.example.it
PORT=5302
SECRET=IT_01_84
REMOTE_PREFIX=22284

[TRUNK_01_91]
HOST=svxref-sicilia.example.it
PORT=5302
SECRET=IT_01_91
REMOTE_PREFIX=22291
```

All other regional configs follow the same pattern: set `LOCAL_PREFIX` to the
region's 5-digit MCC-prefixed code from the inventory table (`222` + the 2-digit
regional code) and add one `[TRUNK_xx_yy]` section for each of the other
19 regions, where `xx` and `yy` are the sorted pair of region codes (lower code
first, keeping the 2-digit form for readability of the section name). Both
sides of a trunk link must use the **same section name** — for example, the
link between regions 02 and 21 is named `[TRUNK_02_21]` on both the SARDEGNA
and LOMBARDIA reflectors, but each side sets `REMOTE_PREFIX` to the peer's
full 5-digit prefix (e.g. `22221` for Lombardia). Use the matching `SECRET`
from the convention above.

---

## Cluster TGs

The `CLUSTER_TGS` setting enables nationwide talk groups that are broadcast to
**all** trunk peers regardless of prefix ownership. In this deployment, TG 222
(the Italian national call channel in DMR numbering) is configured as a cluster
TG on every reflector:

```ini
CLUSTER_TGS=222
```

When any client on any regional reflector keys up on TG 222, the audio is sent
to all 19 other reflectors simultaneously. Unlike prefix-based TGs (which route
to a single owning reflector), cluster TGs have no owner — any reflector can
originate a transmission. Talker arbitration works the same way (nonce
tie-break) if two operators key up simultaneously.

All reflectors in the mesh must list the same `CLUSTER_TGS` value.

---

## Reaching international TGs (optional)

The mesh above is **purely national** — every region reaches every other Italian
region directly, so no extra routing is needed *inside* Italy. But a region's
prefix table only contains Italian `222…` prefixes, so a TG owned **abroad**
(another country's MCC space, e.g. Germany `262`) matches nothing and stays
local. To let Italian clients reach foreign TGs, two pieces are needed:

1. **Designate an international gateway.** Pick one reflector to join the
   international mesh by trunking to foreign countries. Name those links with the
   international standard `TRUNK_<ISO>_<ISO>`, ISO codes alphabetical — e.g.
   `TRUNK_DE_IT` to Germany, `TRUNK_FR_IT` to France (see the README, section 4).

2. **Point each region at the gateway with `ROUTABLE_PREFIXES`.** On every
   region's trunk section *facing the gateway*, declare the foreign prefixes as
   reachable via it:

   ```ini
   # On each region, in the trunk section facing the international gateway:
   ROUTABLE_PREFIXES=262,208      # DE, FR, … reachable via the gateway
   ```

Now a regional client keying e.g. `26201` (a German TG) matches the routable
`262` on the gateway trunk, so the region forwards it to the gateway, which
relays it into the international mesh toward Germany. The return audio comes back
via peer interest.

Notes:

- Each prefix you want reachable must be named — there is **no** automatic
  propagation. A region that is **not** in the national full mesh (a single
  reflector whose only trunk is the gateway) can instead set
  `ROUTABLE_PREFIXES=*`, a default route to everything via that one uplink. Do
  **not** use `*` on a region that has multiple trunks (it logs a startup
  warning).
- `BLACKLIST_TGS` is evaluated first: a blacklisted TG is never forwarded,
  regardless of `ROUTABLE_PREFIXES`.

See the README
[Routable prefixes](../README.md#5-routable-prefixes--hierarchical-and-non-full-mesh-topologies-optional)
section and [`TOPOLOGY_EXAMPLES.md`](TOPOLOGY_EXAMPLES.md) §2 for the full
mechanism and diagrams.

---

## Concurrent conversations

There is **no hardcoded limit** on concurrent QSOs over the trunk. The trunk
between each pair of reflectors is a single TCP connection that multiplexes all
active TGs simultaneously — each `MsgPeerAudio` frame is tagged with the TG
number, so any number of TGs can carry audio at the same time.

The only per-TG rule is that **one talker is allowed per TG at a time**
(enforced by the arbitration logic). Multiple TGs can all have active talkers
simultaneously without interfering.

### Practical bandwidth estimate

| Codec | Bitrate per active TG | Concurrent TGs on a 1 Mbps trunk |
|-------|-----------------------|-----------------------------------|
| OPUS  | ~8–16 kbps            | ~60–125                           |
| GSM   | ~13 kbps              | ~75                               |

In this Italian deployment, even if all 20 regional TGs carried a simultaneous
QSO, the total trunk load per reflector would be well under 1 Mbps. Bandwidth
is not a concern at this scale; the limiting factor in practice is the number of
licensed operators active at any given time.

---

## Satellite reflectors (optional)

Larger regions may want to run additional reflector instances as satellites
(e.g. one per province) to distribute the client load without adding more trunk
mesh connections. A satellite connects to its regional reflector and relays all
traffic — remote reflectors see satellite clients as if they were connected
directly to the regional reflector.

**Regional reflector** (e.g. Lazio) — add a `[SATELLITE]` section:
```ini
[SATELLITE]
LISTEN_PORT=5303
SECRET=lazio_satellite_secret
```

To give each provincial satellite its own credential (so a leak in one
province doesn't impersonate another), use per-id entries — the id
after `SECRET_` is matched against the satellite's `SATELLITE_ID`
(charset `[A-Za-z0-9-]+`, no underscore):

```ini
[SATELLITE]
LISTEN_PORT=5303
SECRET=lazio_satellite_secret           # fallback (optional)
SECRET_sat-roma=secret_for_roma
SECRET_sat-viterbo=secret_for_viterbo
```

Per-id wins with no fallback on HMAC mismatch. Keeping `SECRET=` is
backward-compatible: any satellite without a per-id entry uses it.

**Satellite** (e.g. Roma province):
```ini
[GLOBAL]
SATELLITE_OF=svxref-lazio.example.it
SATELLITE_PORT=5303
SATELLITE_SECRET=lazio_satellite_secret
SATELLITE_ID=sat-roma
```

The satellite does not set `LOCAL_PREFIX`, `REMOTE_PREFIX`, or any `[TRUNK_xx_yy]`
sections. It only needs `LISTEN_PORT=5300` for its local SvxLink clients. Also
open port `5303` inbound on the regional reflector firewall.

---

## Per-region config checklist

1. Set `LOCAL_PREFIX` to the 5-digit MCC-prefixed code from the table above
   (e.g. Lazio = `22201`, not the bare `01`).
2. Add one `[TRUNK_xx_yy]` section for **every other region** (19 sections total),
   where `xx` and `yy` are the sorted pair of region codes (lower first). Both
   sides of a trunk link must use the **same** `[TRUNK_xx_yy]` section name.
3. Use the same `SECRET` value as the matching section on the peer — mismatched
   secrets will prevent the trunk from connecting.
4. Open TCP port `5302` inbound in the firewall (trunk) and `5300` inbound (clients).
5. If accepting satellites, also open TCP port `5303` inbound.
6. Ensure `HOST` resolves to the peer's public IP from the server's network.
