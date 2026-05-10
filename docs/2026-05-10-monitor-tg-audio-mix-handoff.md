# Monitor-TG Audio Mixing — Handoff Notes

**Date:** 2026-05-10 (updated 2026-05-10 with §4 verification results)
**Status:** Diagnostic handoff. Verified facts only — no proposed design. §4 open questions now resolved.
**Source:** drafted by Claude in a session rooted in the SvxReflectorDashboard (rails) repo, with read access to this tree but without the long-term context an engineer working in `geureflector/` daily would have. **Hand off to a session with full GeuReflector context to design and implement the fix.**

---

## 1. Bug report (verbatim, Italian, from chat with Audric IW1GEU)

> **Fabio IZ2BKS:** «Scusa Audric, hò notato che dall'ultimo aggiornamento i tg che ci sono in monitor sul nodo di svxlink i flussi audio si mescolano. Ti spiego in monitor hò 1++ 9+ 110 112 ero in qso via radio sul TG9 poi ha inziato a parlare il 110 e i due TG si sono mescolati tra loro diventando incompernsibili.»
>
> **Fabio IZ2BKS:** «Stò effettuando altri test su altri ponti poi ti dico se anche loro sono cosi.»
>
> **Fabio IZ2BKS:** «Confermo che anche su tutti i nodi i TG si mescolano.»
>
> **Audric IW1GEU:** «occaxxo… riesci a darmi un log? stai parlando di bridge? o di nodi collegati al reflector?»
>
> **Fabio IZ2BKS:** «nodi collegati. Solo dall'ultimo update lo fa.»

Translation: with `MONITOR_TGS=1++,9+,110,112` and `SELECT_TG=9`, while in QSO on TG 9 a talker on TG 110 caused TG 9 + TG 110 audio to play simultaneously and become incomprehensible. Reproduces on every connected svxlink node, started immediately after the deploy that ships v1.3.10.

---

## 2. Verified facts

Each fact below is anchored to a specific file/line; I read the source. Line numbers as of HEAD on 2026-05-10.

### 2.1 v1.3.10 changed UDP fanout filters at SEVEN sites (corrected 2026-05-10)

Commit `92d9a63` ("feat(peer): passive-monitor support via MsgPeerTgInterest"). Originally the handoff doc claimed only the three `Reflector.cpp` sites were changed and the four trunk/satellite sites in old §2.2 were pre-existing. Subsequent `git blame` showed all four trunk/satellite sites also trace to `92d9a63`. Full corrected table:

| Site | Before | After (since 92d9a63) |
|---|---|---|
| `Reflector.cpp:1409` (local UDP audio fanout, `udpDatagramReceived`) | `TgFilter(tg)` | `TgFilter(tg) \|\| TgMonitorFilter(tg)` |
| `Reflector.cpp:1576` (local talker-stop UDP flush, `onTalkerUpdated`) | `TgFilter(tg)` | OR-filter |
| `Reflector.cpp:2854` (trunk talker-stop UDP flush, `onTrunkTalkerUpdated`) | `TgFilter(tg)` | OR-filter |
| `TrunkLink.cpp:1161` (trunk → local UDP audio, `handleMsgPeerAudio`) | `TgFilter(local_tg)` | OR-filter |
| `TrunkLink.cpp:1199` (trunk → local UDP flush, `handleMsgPeerFlush`) | `TgFilter(local_tg)` | OR-filter |
| `SatelliteClient.cpp:365, 377` (satellite UDP audio + flush) | `TgFilter(msg.tg())` | OR-filter |
| `SatelliteLink.cpp:379, 399` (satellite UDP audio + flush) | `TgFilter(tg)` | OR-filter |

The commit message says "TgMonitorFilter applied at every local UDP fanout (audio + MsgUdpFlushSamples)" and "Strict superset: no existing delivery path is removed." The "every" was literal — all UDP delivery paths to local clients except the twin link (see §2.2 below).

### 2.2 TwinLink UDP fanout uses bare `TgFilter` — does NOT include monitor OR

Confirmed during §4 verification. The only UDP fanout path NOT touched by `92d9a63`:

| Site | Filter | Blame |
|---|---|---|
| `TwinLink.cpp:622` (twin → local UDP audio, `handleMsgPeerAudio`) | `TgFilter(msg.tg())` only | `18498932` (2026-04-15) |
| `TwinLink.cpp:635` (twin → local UDP flush, `handleMsgPeerFlush`) | `TgFilter(msg.tg())` only | `18498932` (2026-04-15) |

Implication: the twin path delivers audio only to clients on the active TG, never to monitors. Twin cannot cause or amplify the mixing symptom on its own. The asymmetry (twin uses bare filter; trunk/satellite use OR-filter) is a design-decision input — whatever fix lands here may want to align twin too, separately.

### 2.3 TCP control fanouts to monitor clients were also OR-filtered, NOT changed by v1.3.10

Read; the v1.3.10 diff does not touch these:

| Site | Message | Filter | OR-filter introduced |
|---|---|---|---|
| `Reflector.cpp:1566` | `MsgTalkerStop` (local) | `TgFilter(tg) \|\| TgMonitorFilter(tg)` | `fca7c33` 2026-03-20 (initial) |
| `Reflector.cpp:1595` | `MsgTalkerStart` (local) | same | `fca7c33` 2026-03-20 (initial) |
| `Reflector.cpp:2848` | `MsgTalkerStop` (trunk) | same | `accb4b36` 2026-03-28 |
| `Reflector.cpp:2870` | `MsgTalkerStart` (trunk) | same | `accb4b36` 2026-03-28 |

Blame nuance (resolves §4.1): the trunk-side TCP filters were not original — added 2026-03-28 by `accb4b36`. Still ~6 weeks before v1.3.10 (2026-05-08), so the implicit "svxlink already auto-switches via `TalkerStart`" assumption holds.

### 2.4 `MsgUdpAudio` carries no TG field — verified in both trees

GeuReflector `src/svxlink/reflector/ReflectorMsg.h` (the `MsgUdpAudio` block):
```cpp
class MsgUdpAudio : public ReflectorUdpMsgBase<101> {
    ...
    ASYNC_MSG_MEMBERS(m_audio_data)
private:
    std::vector<uint8_t> m_audio_data;
};
```

Upstream svxlink `src/svxlink/reflector/ReflectorMsg.h:1423-1444`: identical shape, audio bytes only. Same for `MsgUdpFlushSamples` — no TG.

### 2.5 Upstream svxlink `ReflectorLogic` has no per-TG gate on incoming UDP audio

Upstream svxlink `src/svxlink/svxlink/ReflectorLogic.cpp:2194-2209`:

```cpp
case MsgUdpAudio::TYPE: {
    MsgUdpAudio msg;
    if (!msg.unpack(ss)) { ...return; }
    if (!msg.audioData().empty()) {
        gettimeofday(&m_last_talker_timestamp, NULL);
        m_dec->writeEncodedSamples(
            &msg.audioData().front(), msg.audioData().size());
    }
    break;
}
```

A single `m_dec` (Opus decoder). Every audio frame is fed to it, no TG check (impossible — TG isn't on the wire).

### 2.6 Upstream svxlink implements `++`/`+`/plain priority entirely client-side

Upstream `src/svxlink/svxlink/ReflectorLogic.h:188-195`:
```cpp
struct MonitorTgEntry {
    uint32_t tg;
    uint8_t  prio;
    uint16_t timeout;
    MonitorTgEntry(uint32_t tg=0) : tg(tg), prio(0), timeout(0) {}
    bool operator<(const MonitorTgEntry& mte) const { return tg < mte.tg; }
    bool operator==(const MonitorTgEntry& mte) const { return tg == mte.tg; }
};
```

Upstream `ReflectorLogic.cpp:467-489` parses `MONITOR_TGS=…` and increments `mte.prio` for each trailing `+`.

Upstream `ReflectorLogic.cpp:1877-1918` (`handleMsgTalkerStart`):
```cpp
if (m_tg_select_timeout_cnt == 0) {                      // selected TG idle
    selectTg(msg.tg(), "tg_remote_activation", ...);
}
else if (m_use_prio) {
    // look up selected_tg.prio and talker_tg.prio in m_monitor_tgs
    if ((talker_tg_it != m_monitor_tgs.end()) &&
        (talker_tg_it->prio > selected_tg_prio)) {
        selectTg(msg.tg(), "tg_remote_prio_activation", ...);
    }
}
```

`selectTg` sends `MsgSelectTG` to the reflector.

### 2.7 `MsgTgMonitor` (wire 107) carries no priority

GeuReflector `ReflectorMsg.h:942-954`:
```cpp
class MsgTgMonitor : public ReflectorMsgBase<107> {
    ...
    ASYNC_MSG_MEMBERS(m_tgs);
private:
    std::set<uint32_t> m_tgs;
};
```

Upstream identical. Priority info from svxlink is not transmitted to the reflector.

### 2.8 The `MsgPeerTgInterest` machinery is in place

Verified: `Reflector::scheduleTgInterestUpdate` (added by `92d9a63`), `Reflector::aggregatePeerInterestsForLink`, edge-trigger on client connect/disconnect/`MsgSelectTG`/`MsgTgMonitor`/trunk hello, 60 s heartbeat refresh (`e82be56`), prefix-routed re-advertise. The receive side writes `mapTgIn(tg)` into `m_peer_interested_tgs` (timestamp-keyed). Code paths that consume this state (e.g. `isPeerInterestedTG`, `shouldRelayInbound`, `forwardTrunkAudioToOtherTrunks`) are present in `Reflector.cpp` and `TrunkLink.cpp`.

### 2.9 Symptom mechanism (inferred from 2.4 + 2.5)

`MsgUdpAudio` has no TG, so the receiver cannot demultiplex. Pre-v1.3.10, the reflector guaranteed at most one TG's frames on a given client's socket at a time. Post-v1.3.10 (per fact 2.1), a client whose `monitoredTGs` includes multiple TGs receives interleaved frames from any of them when active simultaneously. Two encoder sessions feeding one stateful Opus decoder produces garbled PCM — the audible "mixing" Fabio reports.

This is the mechanism. It is consistent with the "started immediately after v1.3.10" timing reported by Fabio. I have not independently captured Fabio's traffic; the bug report is the only direct evidence of the symptom.

---

## 3. Conversation-derived constraints (from Audric)

These shape the design space for the fix. Quoted/paraphrased from this session's conversation, not from code:

1. **GeuReflector must work with unmodified upstream svxlink nodes.** No protocol extensions that require an svxlink-side change. (Quote: "geureflector MUST work with unmodified svxlink nodes.")
2. **Original svxlink priority semantics (`++`/`+`/plain) must continue to work.** (Quote: "original priority is a must.")
3. **No 'dirty fix.'** Audric explicitly rejected a plain revert of the v1.3.10 UDP-fanout addition as the proposed fix. (Quote: "no immediate dirty fix" — repeated.)
4. **GeuReflector source ownership.** GeuReflector ships only the reflector binary; it does not fork or modify svxlink itself. (Quote: "geureflector repo never touched svxlink code (shouldn't have). Only svxreflector should have expanded functionality.")

---

## 4. Verification results (resolved 2026-05-10)

Each item is the original open question, with the result of a follow-up read+blame pass. Items 1–6 verified against HEAD `e82be56`.

1. **TCP `MsgTalker{Start,Stop}` OR-filter pre-existence — CONFIRMED, with one nuance.** Local sites (`Reflector.cpp:1566`, `1595`) trace to `fca7c33` 2026-03-20 (initial). Trunk sites (`Reflector.cpp:2848`, `2870`) trace to `accb4b36` 2026-03-28 — not original, but ~6 weeks pre-v1.3.10. The "svxlink auto-switches via `MsgTalkerStart`" assumption holds. See updated §2.3 table.

2. **Missed UDP fanout sites — YES, the trunk/satellite OR-filters traced to v1.3.10 too.** Originally this doc placed `TrunkLink.cpp:1161, 1199` and `SatelliteClient.cpp:365, 377` and `SatelliteLink.cpp:379, 399` as "pre-existing OR-filters." Blame proves they were all introduced by `92d9a63` (v1.3.10). §2.1 has been corrected to list all seven `92d9a63` sites. `TwinLink.cpp:622, 635` (added `18498932` 2026-04-15) are the only UDP fanout sites that use bare `TgFilter` and are documented in §2.2. Other `broadcastUdpMsg` / `sendUdpMsg` call sites surveyed: `Reflector.cpp:1506` is `MsgUdpAllSamplesFlushed` (per-client direct send, not multi-TG fanout); `ReflectorClient.cpp:855` is the per-client TG-switch flush (excludes self, single-TG); `Reflector.cpp:1271, 1324` are heartbeat. None are multi-TG audio fanouts that bypass the corrected §2.1 / §2.2 tables.

3. **Twin-link receive path — does NOT contribute to mixing.** Both `TwinLink::handleMsgPeerAudio` and `handleMsgPeerFlush` use bare `TgFilter` (no monitor OR), so twin audio never reaches monitor TGs at all. See §2.2. Asymmetry vs. trunk/satellite is a design input for whoever picks up the fix.

4. **`TGHandler` "selected-TG busy" accessors reflect live state — CONFIRMED.** `TGHandler.cpp:252-388`:
   - `talkerForTG(tg)` → `m_id_map[tg]->talker` direct lookup, returns `0` if no talker.
   - `TGForClient(client)` → `m_client_map[client]->id` direct lookup, returns `0` if not selected.
   - `hasTrunkTalker(tg)` → `m_trunk_talkers.count(tg) > 0`.

   No caching, no debouncing — safe to use synchronously at UDP fanout time for any "selected TG busy" gate.

5. **`test_39_passive_monitor_multihop` — EXISTS at `tests/test_trunk.py:2662`.** Skipped unless reflector "d" is in the topology. Asserts a passive monitor on reflector-a hears audio from TG `121951` (owned by d) via gateway b, with no PTT bootstrapping. Note: a separate `test_39_rx_debounce_invariant` in `tests/test_mqtt_deltas.py:415` is unrelated — different file, different test-number namespace.

6. **Bridges — no in-tree bridge subsystem.** `grep -i bridge` over the reflector source returned nothing. Bridges in this ecosystem (e.g. svxlink↔DMR/YSF) connect as ordinary SvxLink clients and are subject to exactly the same `TgFilter`/`TgMonitorFilter` paths as any other client. No separate code path means no separate constraint.

7. **Field-log capture from Fabio — still pending.** Out of scope for code verification; tracked here for completeness. Symptom/timing/protocol convergence on §2.9 remains the working hypothesis; a log would harden it.

---

## 5. What I deliberately did NOT include

A specific design proposal. The conversation that led here narrowed in on a per-client UDP audio mux as one candidate, but Audric's last directive was to defer the design decision to a session with full GeuReflector context. The verified facts in §2 and the constraints in §3 should be enough for that session to evaluate options (mux, partial revert, protocol bump, something else) on its own merits without inheriting my preferred shape.

---

## 6. Reading list (what I actually read)

- `geureflector/src/svxlink/reflector/Reflector.cpp` — fanout sites and v1.3.10 changes
- `geureflector/src/svxlink/reflector/Reflector.h`
- `geureflector/src/svxlink/reflector/ReflectorClient.cpp:865-895, 1390-1400` — `handleTgMonitor`, monitor TG status update
- `geureflector/src/svxlink/reflector/ReflectorClient.h:185-225, 445-455` — Filters, accessors
- `geureflector/src/svxlink/reflector/ReflectorMsg.h:920-955` — `MsgTgMonitor`, `MsgUdpAudio` analogue
- `geureflector/src/svxlink/reflector/TrunkLink.cpp:1140-1215` — peer audio + flush handlers
- `geureflector/src/svxlink/reflector/SatelliteClient.cpp` (grep only)
- `geureflector/src/svxlink/reflector/SatelliteLink.cpp` (grep only)
- Upstream svxlink `src/svxlink/svxlink/ReflectorLogic.{h,cpp}` (full read of receive paths)
- Upstream svxlink `src/svxlink/reflector/ReflectorMsg.h:1423-1444` — `MsgUdpAudio`
- Commits `92d9a63` (full diff), `e82be56` (heartbeat fix)
