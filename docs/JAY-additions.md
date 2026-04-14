# jayReflector Additions

This document describes features merged into GeuReflector from
[**jayReflector**](https://github.com/dj1jay/jayReflector) by
**DJ1JAY / FM-Funknetz**.

## Credits

All design and original implementation of the features below is the work of
DJ1JAY. They were ported into this repository with light adaptation to fit the
GeuReflector code paths (in particular: the in-process `MqttPublisher` is
retained instead of the external `jay-mqtt-daemon.py`).

If you build on these features, please credit DJ1JAY / FM-Funknetz and
consult the upstream jayReflector repository for context and additional
material that was not merged here.

## What was merged

| Area | Key / Message | Purpose |
|------|---------------|---------|
| Filter syntax | `TgFilter` (header-only, `src/svxlink/reflector/TgFilter.h`) | Flexible TG matching: exact (`26200`), prefix (`24*`), range (`2427-2438`), comma-separated |
| Per-trunk config | `BLACKLIST_TGS` | TGs never carried in either direction on this link |
| Per-trunk config | `ALLOW_TGS` | If non-empty, only these TGs are exchanged on this link |
| Per-trunk config | `TG_MAP=peer:local,...` | Bidirectional TG remap; mapped TGs bypass the prefix routing check on this link |
| Per-trunk config | `PEER_ID` | Stable identifier advertised in the trunk hello (defaults to section name); used as the MQTT topic component on the receiving side |
| Global config | `MQTT_NAME` (in `[MQTT]`) | Appended to `TOPIC_PREFIX` so each reflector publishes under a unique sub-tree (`<TOPIC_PREFIX>/<MQTT_NAME>/...`) |
| Wire protocol | `MsgTrunkNodeList` (type **121**) | Periodic node list (callsign + current TG, plus optional lat/lon/QTH) sent peer-to-peer when the local client list changes; receiver re-publishes it via MQTT |
| Wire protocol | `MsgTrunkFilter` (type **122**) | Optional satellite-side TG-filter advertisement so a satellite can subscribe to only a subset of the parent's TGs |
| PTY commands | `TRUNK MUTE \| UNMUTE <section> <callsign>` | Live mute of a callsign per trunk link; muted callsigns' inbound audio is dropped |
| PTY commands | `TRUNK RELOAD [<section>]` | Re-reads `BLACKLIST_TGS`, `ALLOW_TGS`, `TG_MAP` from the live config without restarting |
| PTY commands | `TRUNK STATUS [<section>]` | Dumps a one-line status summary per trunk link to the PTY |

## Backward compatibility

- New message types (121, 122) are silently ignored by reflector instances that
  don't know about them. Mixed deployments (jayReflector ↔ this branch ↔
  upstream GeuReflector v3) continue to interoperate for the legacy message
  set (115–120).
- All new config keys are optional. Reflectors with no `BLACKLIST_TGS`,
  `ALLOW_TGS`, `TG_MAP`, `PEER_ID`, or `MQTT_NAME` behave exactly as before.

## Filter semantics

For a given TG on a given trunk link, traffic is allowed only if:

```
not isBlacklisted(tg)            // blacklist applies in both directions
and isAllowed(tg)                // empty allow filter = allow all
and (
    explicit TG_MAP entry        // mapped TGs always pass
    or isSharedTG(tg)            // standard prefix-based ownership routing
    or isClusterTG(tg)           // cluster TG broadcast
    or isPeerInterestedTG(tg)    // peer recently advertised interest
)
```

For incoming messages the reflector applies `TG_MAP` first (peer wire TG →
local TG) and then performs all subsequent checks against both the wire and
local TG so the operator can blacklist either form.

## Differences from upstream jayReflector

The following jayReflector items were **not** merged on request:

- **External MQTT daemon (`jay-mqtt-daemon.py`)** — this branch keeps the
  existing in-process `MqttPublisher`. As a result, some MQTT topics differ
  in suffix (this branch publishes node lists as `nodes/local` and
  `nodes/<peer_id>`; jayReflector's external daemon uses a slightly different
  layout). The `MQTT_NAME` and `PEER_ID` keys behave identically.

If you want any of the deferred items merged, see the upstream repository
linked above and open an issue or PR on this repository.

## Where to look in the code

| File | What to look for |
|------|------------------|
| `src/svxlink/reflector/TgFilter.h` | Filter parser |
| `src/svxlink/reflector/ReflectorMsg.h` | `MsgTrunkNodeList` (121), `MsgTrunkFilter` (122) |
| `src/svxlink/reflector/TrunkLink.{h,cpp}` | Per-link filters, mapping, mute, `reloadConfig`, `statusLine`, `sendNodeList`, `handleMsgTrunkNodeList`, `peerId()` |
| `src/svxlink/reflector/Reflector.{h,cpp}` | `scheduleNodeListUpdate`, `sendNodeListToAllPeers`, `onPeerNodeList`, PTY `TRUNK` command branch, `MQTT_NAME` topic prefix |
| `src/svxlink/reflector/MqttPublisher.{h,cpp}` | `publishLocalNodes`, `publishPeerNodes` |
| `src/svxlink/reflector/svxreflector.conf.in` | Config-template documentation for every new key |
| `tests/test_trunk.py` | Tests 19–27 cover the merged features |
