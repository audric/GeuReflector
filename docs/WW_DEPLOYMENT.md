# Worldwide Deployment Example

This document describes a global deployment using one GeuReflector instance per
country, trunked together in a full-mesh topology.

## Why the trunk is necessary

A standard SvxReflector instance is a single centralized server. Without trunking,
a worldwide deployment has only two options:

**Option A — one global reflector:**
every country connects its nodes to a single server. This works, but only
**one talk group can be active at a time across the entire planet**. While
Germany is in QSO on the national TG, no other country can have a simultaneous
independent conversation. Every transmission from every country travels through
the same server — across continents — and a single server outage takes the
whole network down. Latency from the far side of the globe is also noticeable.

**Option B — independent national reflectors bridged by SvxLink instances:**
it is possible to link two reflectors together by running a SvxLink instance
that connects to both as a client node, acting as an audio bridge. However,
each such bridge instance can only handle one TG at a time, and one instance
is needed per pair of reflectors. For 25 countries in full mesh that is 300
SvxLink bridge instances just to cover one shared TG — and multiplied again
for every additional TG that needs to be shared. The operational complexity,
resource usage, and failure surface of managing hundreds of bridge processes
makes this approach impractical at global scale.

**With GeuReflector trunks:**
each country runs its own independent reflector (resilience, national
autonomy, local latency), and the trunk links connect them so they can share
TGs selectively. All national TGs can carry simultaneous independent QSOs,
while a dedicated worldwide TG is available to all. The trunk handles talker
arbitration automatically when two countries try to use a shared TG at the
same time.

| | Single reflector | Independent reflectors | GeuReflector trunk mesh |
|---|---|---|---|
| Simultaneous national QSOs | No — one TG at a time | Yes, but one bridge per TG per pair | Yes |
| Inter-country communication | Yes | Yes, but 300+ bridge processes | Yes |
| National autonomy | No | Yes | Yes |
| Single point of failure | Yes | No | No |
| Local client latency | No — all traffic crosses oceans | Yes | Yes |

---

## TG numbering

The TG numbers used here are **inspired by the DMR Mobile Country Code (MCC)
numbering scheme**, which is already well thought out, widely adopted, and
recognised across the amateur radio community. Reusing this structure avoids
reinventing a numbering plan and makes the allocation immediately familiar to
operators.

**This SvxLink/GeuReflector system is entirely independent of DMR and is not
connected to any DMR network.** The TG numbers are used here purely as a
convenient and meaningful identifier space — no DMR radio, hotspot, or
repeater is involved.

Each country is assigned the 3-digit MCC as its `LOCAL_PREFIX`. A country's
national TG is simply its MCC (e.g. TG `222` for Italy, TG `262` for Germany).
Sub-national TGs extend the MCC (e.g. `22201` for Lazio, `2621` for a German
regional TG) and are owned by that country's reflector via longest-prefix
match.

Both `LOCAL_PREFIX` and `REMOTE_PREFIX` accept a comma-separated list of
prefixes, so a single reflector can own multiple MCCs (useful for countries
that hold more than one MCC allocation, such as the USA `310,311,312,313,314,
315,316` or the UK `234,235`).

---

## Reflector inventory

A representative selection of countries; extend or trim as needed. The example
uses 25 reflectors in full mesh.

| Country         | National TG | LOCAL_PREFIX            | Suggested hostname               |
|-----------------|-------------|-------------------------|----------------------------------|
| United States   | 310         | 310,311,312,313,314,315,316 | svxref-us.example.net       |
| Canada          | 302         | 302                     | svxref-ca.example.net            |
| Mexico          | 334         | 334                     | svxref-mx.example.net            |
| Brazil          | 724         | 724                     | svxref-br.example.net            |
| Argentina       | 722         | 722                     | svxref-ar.example.net            |
| United Kingdom  | 234         | 234,235                 | svxref-uk.example.net            |
| Ireland         | 272         | 272                     | svxref-ie.example.net            |
| France          | 208         | 208                     | svxref-fr.example.net            |
| Germany         | 262         | 262                     | svxref-de.example.net            |
| Netherlands     | 204         | 204                     | svxref-nl.example.net            |
| Belgium         | 206         | 206                     | svxref-be.example.net            |
| Switzerland     | 228         | 228                     | svxref-ch.example.net            |
| Austria         | 232         | 232                     | svxref-at.example.net            |
| Italy           | 222         | 222                     | svxref-it.example.net            |
| Spain           | 214         | 214                     | svxref-es.example.net            |
| Portugal        | 268         | 268                     | svxref-pt.example.net            |
| Poland          | 260         | 260                     | svxref-pl.example.net            |
| Czech Republic  | 230         | 230                     | svxref-cz.example.net            |
| Sweden          | 240         | 240                     | svxref-se.example.net            |
| Norway          | 242         | 242                     | svxref-no.example.net            |
| Finland         | 244         | 244                     | svxref-fi.example.net            |
| Denmark         | 238         | 238                     | svxref-dk.example.net            |
| Japan           | 440         | 440,441                 | svxref-jp.example.net            |
| Australia       | 505         | 505                     | svxref-au.example.net            |
| New Zealand     | 530         | 530                     | svxref-nz.example.net            |

---

## Topology

25 reflectors × 24 trunk links each = **300 trunk TCP connections** total
(full mesh). Every pair shares a unique `SECRET`. Port `5302` is used for all
trunk connections; port `5300` remains for SvxLink client nodes.

At global scale the main concern is not bandwidth but the combinatorial growth
of the mesh as new countries join. A full mesh stays practical up to roughly
40–50 reflectors; beyond that, a regional-hub topology (continental trunks
with satellites) is worth considering.

---

## Shared secrets convention

Each pair of reflectors shares one secret. Use a naming convention based on
the ISO 3166-1 alpha-2 country codes (alphabetical order) to keep them
organised:

```
WW_AT_BE   # Austria ↔ Belgium
WW_AT_CH   # Austria ↔ Switzerland
WW_DE_FR   # Germany ↔ France
…
WW_NZ_US   # New Zealand ↔ United States
```

Using the ISO codes (rather than the MCCs) keeps the section names short and
readable regardless of how many MCCs a country owns.

---

## Configuration: Germany (complete example)

```ini
[GLOBAL]
LISTEN_PORT=5300
LOCAL_PREFIX=262
CLUSTER_TGS=1,91
HTTP_SRV_PORT=8080
COMMAND_PTY=/dev/shm/reflector_ctrl

[TRUNK_AR_DE]
HOST=svxref-ar.example.net
PORT=5302
SECRET=WW_AR_DE
REMOTE_PREFIX=722

[TRUNK_AT_DE]
HOST=svxref-at.example.net
PORT=5302
SECRET=WW_AT_DE
REMOTE_PREFIX=232

[TRUNK_AU_DE]
HOST=svxref-au.example.net
PORT=5302
SECRET=WW_AU_DE
REMOTE_PREFIX=505

[TRUNK_BE_DE]
HOST=svxref-be.example.net
PORT=5302
SECRET=WW_BE_DE
REMOTE_PREFIX=206

[TRUNK_BR_DE]
HOST=svxref-br.example.net
PORT=5302
SECRET=WW_BR_DE
REMOTE_PREFIX=724

[TRUNK_CA_DE]
HOST=svxref-ca.example.net
PORT=5302
SECRET=WW_CA_DE
REMOTE_PREFIX=302

[TRUNK_CH_DE]
HOST=svxref-ch.example.net
PORT=5302
SECRET=WW_CH_DE
REMOTE_PREFIX=228

[TRUNK_CZ_DE]
HOST=svxref-cz.example.net
PORT=5302
SECRET=WW_CZ_DE
REMOTE_PREFIX=230

[TRUNK_DE_DK]
HOST=svxref-dk.example.net
PORT=5302
SECRET=WW_DE_DK
REMOTE_PREFIX=238

[TRUNK_DE_ES]
HOST=svxref-es.example.net
PORT=5302
SECRET=WW_DE_ES
REMOTE_PREFIX=214

[TRUNK_DE_FI]
HOST=svxref-fi.example.net
PORT=5302
SECRET=WW_DE_FI
REMOTE_PREFIX=244

[TRUNK_DE_FR]
HOST=svxref-fr.example.net
PORT=5302
SECRET=WW_DE_FR
REMOTE_PREFIX=208

[TRUNK_DE_IE]
HOST=svxref-ie.example.net
PORT=5302
SECRET=WW_DE_IE
REMOTE_PREFIX=272

[TRUNK_DE_IT]
HOST=svxref-it.example.net
PORT=5302
SECRET=WW_DE_IT
REMOTE_PREFIX=222

[TRUNK_DE_JP]
HOST=svxref-jp.example.net
PORT=5302
SECRET=WW_DE_JP
REMOTE_PREFIX=440,441

[TRUNK_DE_MX]
HOST=svxref-mx.example.net
PORT=5302
SECRET=WW_DE_MX
REMOTE_PREFIX=334

[TRUNK_DE_NL]
HOST=svxref-nl.example.net
PORT=5302
SECRET=WW_DE_NL
REMOTE_PREFIX=204

[TRUNK_DE_NO]
HOST=svxref-no.example.net
PORT=5302
SECRET=WW_DE_NO
REMOTE_PREFIX=242

[TRUNK_DE_NZ]
HOST=svxref-nz.example.net
PORT=5302
SECRET=WW_DE_NZ
REMOTE_PREFIX=530

[TRUNK_DE_PL]
HOST=svxref-pl.example.net
PORT=5302
SECRET=WW_DE_PL
REMOTE_PREFIX=260

[TRUNK_DE_PT]
HOST=svxref-pt.example.net
PORT=5302
SECRET=WW_DE_PT
REMOTE_PREFIX=268

[TRUNK_DE_SE]
HOST=svxref-se.example.net
PORT=5302
SECRET=WW_DE_SE
REMOTE_PREFIX=240

[TRUNK_DE_UK]
HOST=svxref-uk.example.net
PORT=5302
SECRET=WW_DE_UK
REMOTE_PREFIX=234,235

[TRUNK_DE_US]
HOST=svxref-us.example.net
PORT=5302
SECRET=WW_DE_US
REMOTE_PREFIX=310,311,312,313,314,315,316
```

All other national configs follow the same pattern: set `LOCAL_PREFIX` to the
country's MCC(s) and add one `[TRUNK_XX_YY]` section for each of the other 24
countries, where `XX` and `YY` are the sorted pair of ISO codes (alphabetical
order). Both sides of a trunk link must use the **same section name** — for
example, the link between France and Italy is named `[TRUNK_FR_IT]` on both
reflectors. Use the matching `SECRET` from the convention above.

---

## Cluster TGs

The `CLUSTER_TGS` setting enables worldwide talk groups that are broadcast to
**all** trunk peers regardless of prefix ownership. In this deployment, TGs
`1` and `91` (the DMR worldwide call channels) are configured as cluster TGs
on every reflector:

```ini
CLUSTER_TGS=1,91
```

When any client on any national reflector keys up on TG 1 or 91, the audio is
sent to all 24 other reflectors simultaneously. Unlike prefix-based TGs
(which route to a single owning reflector), cluster TGs have no owner — any
reflector can originate a transmission. Talker arbitration works the same way
(nonce tie-break) if two operators key up simultaneously.

All reflectors in the mesh must list the same `CLUSTER_TGS` value.

Regional cluster TGs (e.g. TG `2` for Europe-wide, TG `3` for North America)
can be added by configuring the same additional cluster TG only on the
reflectors that should participate — but note that `CLUSTER_TGS` is currently
a flat list, so to keep a regional TG off other continents you would need to
omit it from their configs. Every reflector that lists a TG in `CLUSTER_TGS`
will broadcast it to all of its trunk peers.

---

## Concurrent conversations

There is **no hardcoded limit** on concurrent QSOs over the trunk. The trunk
between each pair of reflectors is a single TCP connection that multiplexes
all active TGs simultaneously — each `MsgTrunkAudio` frame is tagged with the
TG number, so any number of TGs can carry audio at the same time.

The only per-TG rule is that **one talker is allowed per TG at a time**
(enforced by the arbitration logic). Multiple TGs can all have active talkers
simultaneously without interfering.

### Practical bandwidth estimate

| Codec | Bitrate per active TG | Concurrent TGs on a 1 Mbps trunk |
|-------|-----------------------|-----------------------------------|
| OPUS  | ~8–16 kbps            | ~60–125                           |
| GSM   | ~13 kbps              | ~75                               |

In this worldwide deployment, even if every national TG and both worldwide
cluster TGs carried simultaneous QSOs, total trunk load per reflector would
remain well under 1 Mbps. Bandwidth is not a concern at this scale; the
limiting factor in practice is the number of licensed operators active on
shared TGs at any given time, and the intercontinental round-trip latency
between peers.

### Latency considerations

Unlike a national deployment, a worldwide mesh has to deal with
intercontinental RTT (typically 150–300 ms across an ocean). This is
transparent to the protocol — TCP-based trunk audio tolerates the extra delay
— but operators on a worldwide TG will hear noticeable one-way delay between
continents. Picking a geographically central server for continental-scale
cluster TGs helps; there is nothing the reflector itself can do to hide
propagation delay.

---

## Satellite reflectors (optional)

Large countries or countries with complex internal TG hierarchies can run
additional reflector instances as satellites, or as full sub-reflectors with
their own prefix (see `DEPLOYMENT_ITALY.md` for an example of a country-level
mesh that could hang off a single "Italy" node in this global mesh).

**National reflector** (e.g. Germany) — add a `[SATELLITE]` section:
```ini
[SATELLITE]
LISTEN_PORT=5303
SECRET=de_satellite_secret
```

**Satellite** (e.g. Bavaria):
```ini
[GLOBAL]
SATELLITE_OF=svxref-de.example.net
SATELLITE_PORT=5303
SATELLITE_SECRET=de_satellite_secret
SATELLITE_ID=sat-bayern
```

The satellite does not set `LOCAL_PREFIX`, `REMOTE_PREFIX`, or any
`[TRUNK_XX_YY]` sections. It only needs `LISTEN_PORT=5300` for its local
SvxLink clients. Also open port `5303` inbound on the national reflector
firewall.

This lets a country scale out internally (one satellite per region or state)
without adding any new trunk connections to the worldwide mesh — remote
reflectors continue to see a single national reflector.

---

## Per-country config checklist

1. Set `LOCAL_PREFIX` to the country's MCC(s) from the table above (comma-
   separated if more than one).
2. Add one `[TRUNK_XX_YY]` section for **every other country** (24 sections
   total for a 25-country mesh), where `XX` and `YY` are the sorted pair of
   ISO 3166-1 alpha-2 codes (alphabetical). Both sides of a trunk link must
   use the **same** `[TRUNK_XX_YY]` section name.
3. Use the same `SECRET` value as the matching section on the peer —
   mismatched secrets will prevent the trunk from connecting.
4. Open TCP port `5302` inbound in the firewall (trunk) and `5300` inbound
   (clients).
5. If accepting satellites, also open TCP port `5303` inbound.
6. Ensure `HOST` resolves to the peer's public IP from the server's network.
7. Keep `CLUSTER_TGS` identical on every reflector in the mesh, otherwise
   worldwide TGs will not be delivered consistently.
