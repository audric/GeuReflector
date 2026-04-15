# Integration Test Suite

## Overview

The integration tests verify the trunk protocol, satellite links, twin (HA-pair) protocol, and end-to-end audio routing by spinning up reflector meshes in Docker Compose and connecting fake peers and clients from a Python test harness.

The suite runs in **two phases**:
1. A 3-reflector trunk mesh exercising `test_trunk.py` (27 tests).
2. A 4-reflector twin topology exercising `test_twin.py` (8 tests).

**Requirements:** Docker, Docker Compose, Python 3.7+ (stdlib only, no pip packages).

## Running

```bash
cd tests
bash run_tests.sh
```

This will:
1. Generate the default configs and `docker-compose.test.yml` from `topology.py`
2. Build and start the 3-reflector mesh
3. Run 27 automated trunk/satellite/MQTT tests (`test_trunk.py`)
4. Enter an interactive prompt to manually test any TG number
5. Tear down the default mesh
6. Regenerate with `--topology twin` (4-reflector twin topology)
7. Rebuild the mesh and run 8 automated twin-protocol tests (`test_twin.py`)
8. Restore the default topology files and tear everything down

To regenerate configs without running tests:

```bash
python3 generate_configs.py                 # default 3-reflector mesh
python3 generate_configs.py --topology twin # 4-reflector twin topology
```

## Test Mesh Topology

Three reflectors form a full-mesh trunk network:

```
reflector-a (prefix 122) ◄──► reflector-b (prefix 121)
       ▲                              ▲
       └──────── reflector-c ─────────┘
                  (prefix 1)
```

**Prefix ownership** determines routing. TG `12200` belongs to reflector-a (prefix `122`), TG `12100` to reflector-b (`121`), and TG `1000` to reflector-c (`1`) — unless a longer prefix matches first. For example, `122xx` matches reflector-a even though reflector-c's prefix `1` is also a prefix of `122xx`, because longest-prefix-match wins.

**Cluster TGs** (222, 999) are broadcast to all peers regardless of prefix ownership.

Two fake trunk peers connect from the test harness:
- **TRUNK_TEST** (prefix `9`) — primary sender for most tests
- **TRUNK_TEST_RX** (prefix `8`) — passive receiver for isolation tests

Two V2 clients are configured on every reflector:
- **N0TEST** / **N0SEND** (group `TestGroup`, password `testpass`)

A satellite server is enabled on reflector-a (port 5303, secret `sat_secret`).

All test configs have `TRUNK_DEBUG=1` enabled for verbose trunk logging during test runs.

### Port Mapping

| Reflector | Client (TCP+UDP) | Trunk (TCP) | HTTP | Satellite |
|-----------|-------------------|-------------|------|-----------|
| a         | 15300             | 15302       | 18080| 15303     |
| b         | 25300             | 25302       | 28080| —         |
| c         | 35300             | 35302       | 38080| —         |

Internal ports inside Docker are always 5300, 5302, 8080, 5303.

## Twin Test Topology

The twin topology (`--topology twin`) exercises the TWIN HA-pair protocol and `PAIRED=1` external trunks. See `docs/TWIN_PROTOCOL.md` for the protocol specification.

```
refa (prefix 222)
  │
  │  [TRUNK_IT_DE] PAIRED=1  (one section, both hosts)
  │  ┌───────────────┐
  ▼  ▼
 ref1 ◄── [TWIN] ──► ref2       (both LOCAL_PREFIX=262)
  │                    │
  └─── refc ───────────┘
       (prefix 333)
```

- **refa** (IT, `LOCAL_PREFIX=222`) — the non-pair side; its single `[TRUNK_IT_DE]` lists both German hosts and uses sticky per-transmission selection with instant failover.
- **ref1, ref2** (DE, `LOCAL_PREFIX=262` on **both**) — the twin pair, linked by a `[TWIN]` section on port 5304 and sharing `TGHandler` state.
- **refc** (`LOCAL_PREFIX=333`) — extra peer so the twin pair also has a normal (non-paired) trunk.

### Twin Port Mapping

| Reflector | Client (TCP+UDP) | Trunk (TCP) | Twin (TCP) | HTTP  |
|-----------|-------------------|-------------|------------|-------|
| refa      | 45300             | 45302       | —          | 48080 |
| ref1      | 46300             | 46302       | 46304      | 49080 |
| ref2      | 47300             | 47302       | 47304      | 50080 |
| refc      | 48300             | 48302       | —          | 51080 |

Internal port for `[TWIN]` is always 5304.

## File Structure

| File | Purpose |
|------|---------|
| `topology.py` | Single source of truth — prefixes, ports, secrets, cluster TGs, test clients. Contains both the default mesh (`REFLECTORS`) and the `TWIN_REFLECTORS` / `TWIN_TRUNKS` definitions. |
| `generate_configs.py` | Generates `configs/*.conf` and `docker-compose.test.yml` from topology. Supports `--topology default` (implicit) and `--topology twin`. |
| `test_trunk.py` | Test harness: fake trunk peers, satellite peer, V2 client, 27 test cases, interactive loop. |
| `test_twin.py` | TWIN-protocol tests (8 cases) using the twin topology. Reuses `ClientPeer` from `test_trunk.py` for the audio-mirror test. |
| `run_tests.sh` | Orchestrator: generate → build → trunk tests → teardown → regenerate twin → build → twin tests → teardown. |
| `configs/` | Generated reflector config files (do not edit manually) |
| `docker-compose.test.yml` | Generated compose file (do not edit manually) |

## Test Harness Components

### TrunkPeer

Simulates a trunk peer connection. Connects via TCP, performs HMAC handshake, and can send/receive all trunk protocol messages (TalkerStart, TalkerStop, Audio, Flush, Heartbeat).

### SatellitePeer

Extends `TrunkPeer` with satellite-specific behavior: connects to the satellite port and sends a hello with `role=SATELLITE`. Authentication is two-way: the satellite proves identity to the parent, then the parent sends a hello reply back so the satellite can verify the parent and enable event forwarding.

### ClientPeer

Simulates a V2 SvxLink client. Performs the full TCP authentication handshake (ProtoVer → AuthChallenge → AuthResponse → AuthOk → ServerInfo), opens a UDP socket for audio, and supports TG selection, TG monitoring, sending/receiving UDP audio frames, and background TCP message draining.

## Test Cases

### Trunk Protocol

| # | Test | What it verifies |
|---|------|-----------------|
| 1 | Mesh connectivity | All trunk links between the 3 reflectors are connected (via `/status`) |
| 2 | Handshake success | Test peer connects, receives hello back with correct prefix and priority nonce |
| 3 | Bad secret rejected | Connection with wrong HMAC secret is dropped |
| 4 | Talker start/stop | `TalkerStart` appears in `/status` active_talkers, `TalkerStop` clears it |
| 5 | Audio relay | Full lifecycle: start → 5 audio frames → flush → stop, all accepted |
| 6 | Cluster TGs accepted | All cluster TGs are accepted regardless of prefix; verified NOT forwarded trunk-to-trunk |
| 7 | Heartbeat keepalive | Sending heartbeats keeps the connection alive after 3 seconds |
| 8 | Disconnect cleanup | Abrupt TCP close clears the talker from `/status` |
| 9 | No trunk-to-trunk audio | Audio from TRUNK_TEST is NOT forwarded to TRUNK_TEST_RX (loop prevention) |

### End-to-End Audio

| # | Test | What it verifies |
|---|------|-----------------|
| 10 | Audio to local client | Trunk audio on a cluster TG reaches a V2 client on the same reflector via UDP |
| 11 | Cross-reflector audio | V2 client on reflector-a talks on a TG owned by reflector-b; V2 client on reflector-b receives the audio via trunk forwarding |

### Satellite Links

| # | Test | What it verifies |
|---|------|-----------------|
| 12 | Satellite handshake | Satellite connects and appears as authenticated in `/status` |
| 13 | Satellite audio to parent | Audio sent by satellite reaches a V2 client on the parent reflector |
| 14 | Satellite receives from parent | Trunk talker audio on the parent is forwarded to the satellite |
| 15 | Satellite audio to trunk peer | Satellite sends audio for a TG owned by another reflector; parent forwards via trunk |
| 16 | Satellite disconnect cleanup | Abrupt satellite disconnect clears it from `/status` |

### Bidirectional Routing

| # | Test | What it verifies |
|---|------|-----------------|
| 17 | Bidirectional trunk conversation | Client-A on reflector-a talks on a TG owned by reflector-b, Client-B on reflector-b receives it; Client-B replies, Client-A receives the return audio (peer interest tracking) |

### MQTT Publishing

| # | Test | What it verifies |
|---|------|-----------------|
| 18 | MQTT talker event | Talker start/stop on a trunk publishes to `<prefix>/talker/<tg>/(start|stop)` |
| 26 | `MQTT_NAME` in topic | Reflector with `MQTT_NAME=mqname-c` publishes node-list under `<prefix>/c/mqname-c/nodes/local` |

### Per-Trunk Filters and Mapping (jayReflector additions)

A third test peer `TRUNK_TEST_FILTER` (prefix `7`, secret `test_secret_filter`)
is configured with `PEER_ID=filter-peer`, `BLACKLIST_TGS=12345`,
`ALLOW_TGS=7*,1220`, and `TG_MAP=7000:1220` so these tests can verify
each filter independently of the other trunk fixtures.

| # | Test | What it verifies |
|---|------|-----------------|
| 19 | `PEER_ID` in hello | Reflector advertises the configured `PEER_ID` (not the section name) in `MsgTrunkHello` |
| 20 | `BLACKLIST_TGS` drops TG | TalkerStart on a blacklisted TG is dropped (not in `/status` active talkers) |
| 21 | `ALLOW_TGS` whitelist | TG matching the whitelist passes; TG outside is dropped |
| 22 | `TG_MAP` remap | TalkerStart on wire TG `7000` is remapped and tracked as local TG `1220` |
| 23 | PTY `TRUNK STATUS` | Command is accepted; `TRUNK_TEST_FILTER` is loaded by the reflector |
| 24 | PTY `TRUNK MUTE` | Audio from a muted callsign is dropped before reaching local clients |
| 25 | PTY `TRUNK RELOAD` | After live `CFG` update of `BLACKLIST_TGS`, `TRUNK RELOAD` re-reads it and the formerly-allowed TG is now blocked |

### Trunk Node-List Exchange

| # | Test | What it verifies |
|---|------|-----------------|
| 27 | `MsgTrunkNodeList` emission | Connecting a V2 client triggers a debounced node-list send to trunk peers; the harness receives a `MsgTrunkNodeList` (type 121) containing the new client |

### Twin Protocol (HA-pair)

Run separately on the twin topology (`test_twin.py`). See `docs/TWIN_PROTOCOL.md` for the protocol.

| # | Test | What it verifies |
|---|------|-----------------|
| 1 | TWIN handshake | Both ref1 and ref2 log a successful `TWIN: hello from partner` with authentication |
| 2 | No auth errors | Neither twin emits HMAC or `local_prefix` mismatch errors on startup |
| 3 | PAIRED trunk on refa | refa's single `[TRUNK_IT_DE]` (PAIRED=1) reports `connected=True` with both hosts reachable |
| 4 | PAIRED return leg | ref1 and ref2 each see their own `[TRUNK_IT_DE]` back to refa as connected |
| 5 | No twin-setup errors | Startup logs on both twins contain no `ERROR[TWIN]` lines |
| 6 | Twin disconnect recovery | Killing ref2 triggers an RX timeout on ref1; restarting ref2 re-handshakes cleanly |
| 7 | PAIRED trunk failover | Killing ref1 does **not** disconnect refa's `[TRUNK_IT_DE]` — sticky selection fails over to ref2 without holdoff |
| 8 | Audio mirror end-to-end | A V2 client on ref1 transmits on TG 26201; a V2 client on ref2 receives UDP audio and a flush marker, exercising the full `TGHandler.talkerUpdated` → `TwinLink.onLocalAudio` → `MsgTrunkAudio` → `broadcastUdpMsg` path |

## Interactive Mode

After the automated tests pass, an interactive prompt lets you test any TG number:

```
TG number (q to quit): 12101
  logs for TG 12101:
    reflector-a: local talker start, local talker stop
    reflector-b: trunk talker start, trunk talker stop
    reflector-c: (no events)
  ✔ TG 12101 routed via trunk
```

The interactive mode connects a V2 client to reflector-a, sends audio on the given TG, then checks docker logs on each reflector for routing evidence.

## Modifying the Topology

All topology is defined in `topology.py`. The file contains two topologies:

- **Default** (`REFLECTORS`, `CLUSTER_TGS`, `TEST_PEER`, etc.) — the 3-reflector mesh used by `test_trunk.py`.
- **Twin** (`TWIN_REFLECTORS`, `TWIN_TRUNKS`, `TWIN_CLUSTER_TGS`) — the 4-reflector twin topology used by `test_twin.py`, including `twin_of` and `paired` fields.

To add a reflector or change prefixes:

1. Edit the relevant constants in `topology.py`
2. Run `python3 generate_configs.py` (default) or `python3 generate_configs.py --topology twin`
3. Run `bash run_tests.sh` to rebuild and test both suites

Do not edit files in `configs/` or `docker-compose.test.yml` directly — they are overwritten by the generator.
