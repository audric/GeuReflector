# Integration Test Suite

## Overview

The integration tests verify the trunk protocol, satellite links, and end-to-end audio routing by spinning up a 3-reflector Docker Compose mesh and connecting fake peers and clients from a Python test harness.

**Requirements:** Docker, Docker Compose, Python 3.7+ (stdlib only, no pip packages).

## Running

```bash
cd tests
bash run_tests.sh
```

This will:
1. Generate configs and `docker-compose.test.yml` from `topology.py`
2. Build and start the 3-reflector mesh
3. Run 17 automated tests
4. Enter an interactive prompt to manually test any TG number
5. Tear down the mesh on exit (or Ctrl-C)

To regenerate configs without running tests:

```bash
python3 generate_configs.py
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

## File Structure

| File | Purpose |
|------|---------|
| `topology.py` | Single source of truth — prefixes, ports, secrets, cluster TGs, test clients |
| `generate_configs.py` | Generates `configs/*.conf` and `docker-compose.test.yml` from topology |
| `test_trunk.py` | Test harness: fake peers, fake clients, 17 test cases, interactive loop |
| `run_tests.sh` | Orchestrator: generate → build → test → teardown |
| `configs/` | Generated reflector config files (do not edit manually) |
| `docker-compose.test.yml` | Generated compose file (do not edit manually) |

## Test Harness Components

### TrunkPeer

Simulates a trunk peer connection. Connects via TCP, performs HMAC handshake, and can send/receive all trunk protocol messages (TalkerStart, TalkerStop, Audio, Flush, Heartbeat).

### SatellitePeer

Extends `TrunkPeer` with satellite-specific behavior: connects to the satellite port and sends a hello with `role=SATELLITE`. Authentication is one-way (satellite proves identity to parent; parent does not send a hello back).

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

All topology is defined in `topology.py`. To add a reflector or change prefixes:

1. Edit `REFLECTORS`, `CLUSTER_TGS`, or other constants in `topology.py`
2. Run `python3 generate_configs.py` to regenerate configs and compose file
3. Run `bash run_tests.sh` to rebuild and test

Do not edit files in `configs/` or `docker-compose.test.yml` directly — they are overwritten by the generator.
