# Redis-backed Config Store

GeuReflector can optionally use a Redis server as the source of truth for user
credentials, password groups, cluster TGs, and per-trunk dynamic settings
(blacklists, allow-lists, TG maps, mute lists). This lets a web dashboard push
config changes to a running reflector without file edits or restarts.

Everything else — ports, certificates, `LOCAL_PREFIX`, trunk peer addresses and
secrets, `[SATELLITE]`, `[MQTT]` — stays in `.conf`. Redis does not replace the
config file; it extends it for the mutable subset.

If `[REDIS]` is absent from `svxreflector.conf`, behavior is identical to today
and there is zero overhead.

---

## Enabling Redis

Add a `[REDIS]` section to `svxreflector.conf` and restart:

```ini
[REDIS]
HOST=127.0.0.1
PORT=6379
PASSWORD=
DB=0
KEY_PREFIX=
```

`UNIX_SOCKET` and `HOST`+`PORT` are mutually exclusive — if `UNIX_SOCKET` is
set it takes precedence:

```ini
[REDIS]
UNIX_SOCKET=/var/run/redis/redis.sock
DB=0
KEY_PREFIX=refl1
```

### Configuration reference

| Field | Default | Description |
|-------|---------|-------------|
| `HOST` | `127.0.0.1` | Redis server hostname or IP |
| `PORT` | `6379` | Redis server TCP port |
| `PASSWORD` | *(empty)* | `AUTH` password; omit or leave blank if not set |
| `DB` | `0` | Redis logical database index |
| `KEY_PREFIX` | *(empty)* | Namespace prefix; when set, all keys become `<prefix>:<key>` |
| `UNIX_SOCKET` | *(empty)* | Path to Redis UNIX socket; takes precedence over `HOST`/`PORT` |
| `TLS_ENABLED` | `0` | Enable TLS (`1`) or not (`0`) |
| `TLS_CA_CERT` | *(empty)* | Path to CA certificate file |
| `TLS_CLIENT_CERT` | *(empty)* | Path to client certificate (mutual TLS only) |
| `TLS_CLIENT_KEY` | *(empty)* | Path to client private key (mutual TLS only) |

Use `KEY_PREFIX` when multiple reflectors share one Redis instance:

```ini
[REDIS]
HOST=redis.example.com
PORT=6379
KEY_PREFIX=refl-italy-1
```

With `KEY_PREFIX=refl-italy-1`, the key `user:SM0ABC` is stored as
`refl-italy-1:user:SM0ABC`.

---

## Override semantics

When `[REDIS]` is present, Redis **fully overrides** the following:

- `[USERS]` — callsign → password-group mapping
- `[PASSWORDS]` — password-group → plaintext password
- `CLUSTER_TGS` in `[GLOBAL]`
- Per-trunk `BLACKLIST_TGS`, `ALLOW_TGS`, `TG_MAP`
- Per-trunk mute lists

There is no merging. Entries in `.conf` for these sections are silently ignored.
The reflector logs a warning for each overridden section at startup:

```
WARN: [USERS] in svxreflector.conf is ignored because [REDIS] is configured.
      Run --import-conf-to-redis to migrate.
```

To move existing `.conf` users and settings into Redis without data loss, run
the importer (see [Migration](#migration)).

---

## Key schema reference

All keys listed below are prefixed with `<KEY_PREFIX>:` when `KEY_PREFIX` is
set.

### Configuration keys (dashboard writes, reflector reads)

| Key | Redis type | Contents |
|-----|-----------|----------|
| `user:<callsign>` | HASH | `{ group, enabled }` |
| `group:<name>` | HASH | `{ password }` |
| `cluster:tgs` | SET | TG numbers as decimal strings |
| `trunk:<section>:blacklist` | SET | TG patterns: exact (`666`), prefix (`24*`), range (`100-199`) |
| `trunk:<section>:allow` | SET | Same syntax as blacklist |
| `trunk:<section>:tgmap` | HASH | `{ peer_tg: local_tg }` decimal string pairs |
| `trunk:<section>:mutes` | SET | Callsigns currently muted on this trunk |

`<section>` is the `[TRUNK_x]` section name from `svxreflector.conf` (e.g.
`TRUNK_1_2`).

The `enabled` field in `user:*` is `"1"` (allowed) or `"0"` (blocked). A
missing `enabled` field is treated as enabled. A user whose group does not have
a matching `group:*` key cannot authenticate; a warning is logged at reload.

### Live-state keys (reflector writes, dashboard reads)

| Key | Redis type | Contents |
|-----|-----------|----------|
| `live:talker:<tg>` | HASH | `{ callsign, started_at, source }` |
| `live:client:<callsign>` | HASH | `{ connected_at, ip, codecs, tg }` |
| `live:trunk:<section>` | HASH | `{ state, last_hb, peer_id }` |

All `live:*` keys carry a 60-second TTL, refreshed every ~30 s by a heartbeat
timer. If the reflector exits uncleanly, stale entries expire automatically
within 60 s.

`source` in `live:talker` is `"local"` for a directly connected client or
`"trunk"` for a remote talker received via trunk.

---

## Pub/sub channels

Two channels are used. Neither carries deltas — the reflector always re-reads
the full scope from Redis on receipt.

| Channel | Direction | Payload |
|---------|-----------|---------|
| `config.changed` | dashboard → reflector | scope token (see table below) |
| `live.changed` | reflector → dashboard | same scope tokens (optional) |

### Scope tokens

| Token | Reflector action |
|-------|-----------------|
| `users` | Re-scan `user:*` and `group:*`, rebuild the auth map |
| `cluster` | Re-read `cluster:tgs`, update the in-memory cluster TG set |
| `trunk:<section>` | Call `reloadConfig()` for that trunk link |
| `all` | All of the above (escape hatch; also sent automatically on Redis reconnect) |

Every write to a configuration key must be followed by a `PUBLISH` to
`config.changed` for the change to take effect immediately. Without the
publish, the reflector picks up the change only at the next scheduled reload
(or never, if there is no scheduled reload configured).

---

## Dashboard operations — cookbook

All examples use plain `redis-cli`. Substitute `redis-cli -p <port> -a <pass>`
as needed. If `KEY_PREFIX=refl1`, prepend `refl1:` to every key.

### Users

**Add a user:**
```bash
redis-cli HSET user:SM0ABC group operators enabled 1
redis-cli PUBLISH config.changed users
```

**Disable a user (reject logins, keep record):**
```bash
redis-cli HSET user:SM0ABC enabled 0
redis-cli PUBLISH config.changed users
```

**Re-enable:**
```bash
redis-cli HSET user:SM0ABC enabled 1
redis-cli PUBLISH config.changed users
```

**Delete a user:**
```bash
redis-cli DEL user:SM0ABC
redis-cli PUBLISH config.changed users
```

**List all users and their group:**
```bash
redis-cli --scan --pattern 'user:*' | while read k; do
    echo -n "$k  "; redis-cli HGETALL "$k"
done
```

### Password groups

**Create a group / change password:**
```bash
redis-cli HSET group:operators password "a strong passphrase"
redis-cli PUBLISH config.changed users
```

**Move a user to a different group:**
```bash
redis-cli HSET user:SM0ABC group admins
redis-cli PUBLISH config.changed users
```

**List groups:**
```bash
redis-cli --scan --pattern 'group:*'
```

### Cluster TGs

**Add a cluster TG:**
```bash
redis-cli SADD cluster:tgs 222
redis-cli PUBLISH config.changed cluster
```

**Remove a cluster TG:**
```bash
redis-cli SREM cluster:tgs 222
redis-cli PUBLISH config.changed cluster
```

**List current cluster TGs:**
```bash
redis-cli SMEMBERS cluster:tgs
```

### Per-trunk filters

Replace `TRUNK_1_2` with your actual `[TRUNK_x]` section name.

**Blacklist a TG (never carry in either direction):**
```bash
redis-cli SADD trunk:TRUNK_1_2:blacklist 666
redis-cli PUBLISH config.changed trunk:TRUNK_1_2
```

**Remove from blacklist:**
```bash
redis-cli SREM trunk:TRUNK_1_2:blacklist 666
redis-cli PUBLISH config.changed trunk:TRUNK_1_2
```

**Set an allow-list (only these TGs exchanged on this link):**
```bash
redis-cli SADD trunk:TRUNK_1_2:allow "24*"
redis-cli SADD trunk:TRUNK_1_2:allow 2624123
redis-cli PUBLISH config.changed trunk:TRUNK_1_2
```

**Remove from allow-list:**
```bash
redis-cli SREM trunk:TRUNK_1_2:allow "24*"
redis-cli PUBLISH config.changed trunk:TRUNK_1_2
```

**Add a TG map entry (peer TG 1 → local TG 2624123):**
```bash
redis-cli HSET trunk:TRUNK_1_2:tgmap 1 2624123
redis-cli PUBLISH config.changed trunk:TRUNK_1_2
```

**Remove a TG map entry:**
```bash
redis-cli HDEL trunk:TRUNK_1_2:tgmap 1
redis-cli PUBLISH config.changed trunk:TRUNK_1_2
```

**View current filters for a trunk:**
```bash
redis-cli SMEMBERS trunk:TRUNK_1_2:blacklist
redis-cli SMEMBERS trunk:TRUNK_1_2:allow
redis-cli HGETALL  trunk:TRUNK_1_2:tgmap
redis-cli SMEMBERS trunk:TRUNK_1_2:mutes
```

### Per-trunk mute lists

**Mute a callsign on a trunk:**
```bash
redis-cli SADD trunk:TRUNK_1_2:mutes ON4ABC
redis-cli PUBLISH config.changed trunk:TRUNK_1_2
```

**Unmute:**
```bash
redis-cli SREM trunk:TRUNK_1_2:mutes ON4ABC
redis-cli PUBLISH config.changed trunk:TRUNK_1_2
```

Note: the PTY command `TRUNK MUTE TRUNK_1_2 ON4ABC` also persists to
`trunk:TRUNK_1_2:mutes` and publishes `config.changed trunk:TRUNK_1_2`
automatically. Either channel (dashboard or PTY) keeps the other in sync.

---

## Live state

The reflector pushes live-state updates to Redis on every significant event:
talker start/stop, client connect/disconnect, trunk state change.

Updates are **not** written directly on the audio code path. Instead, each
event pushes a small command record onto a bounded in-memory FIFO
(`RedisLiveQueue`). A drain timer on the main event loop (every ~50–100 ms)
pops the queue and pipelines the resulting `HSET`/`DEL`/`EXPIRE` calls via
the async hiredis context. This keeps the audio path latency-free even when
Redis is slow or momentarily stalled.

After each drain cycle, the reflector publishes `live.changed <scope>` once
per changed scope if anything was written. Dashboards may subscribe to
`live.changed` for push notifications, or may simply poll `live:*` keys on a
timer.

**If the queue fills** (default capacity: 1 000 entries), the oldest entry is
dropped and a counter (`dropped_live_writes`) is incremented. This metric is
visible in the `/status` JSON (see [Monitoring](#monitoring)).

**On Redis outage:** new live-state events continue to queue in memory up to
the capacity limit, then are dropped. The reflector keeps running and carries
audio normally. See [Failure modes](#failure-modes).

---

## Migration

To import existing `.conf` credentials and settings into Redis, run:

```bash
svxreflector --import-conf-to-redis --config /etc/svxlink/svxreflector.conf
```

Add `--dry-run` to print the Redis commands that would be issued without
contacting Redis:

```bash
svxreflector --import-conf-to-redis --dry-run --config /etc/svxlink/svxreflector.conf
```

**What is imported:**

| `.conf` source | Redis destination |
|---------------|------------------|
| `[USERS]` entries | `user:<callsign>` hashes |
| `[PASSWORDS]` entries | `group:<name>` hashes |
| `CLUSTER_TGS` | `cluster:tgs` set |
| `BLACKLIST_TGS` per `[TRUNK_x]` | `trunk:<section>:blacklist` sets |
| `ALLOW_TGS` per `[TRUNK_x]` | `trunk:<section>:allow` sets |
| `TG_MAP` per `[TRUNK_x]` | `trunk:<section>:tgmap` hashes |

**What is NOT imported:** bootstrap settings (`HOST`, `PORT`, `SECRET`,
`REMOTE_PREFIX`, `LOCAL_PREFIX`, certificate sections, `[SATELLITE]`,
`[MQTT]`, `[REDIS]`). These remain in `.conf`.

The importer is idempotent: it uses `HSET` and `SADD`, not `SETNX`. Running
it twice produces the same result. Re-running after adding new `.conf` entries
will add them without removing existing Redis-only records.

After the import succeeds, remove (or leave in place — they will be ignored)
the `[USERS]`, `[PASSWORDS]`, and `CLUSTER_TGS` entries from `.conf`.

---

## Failure modes

| Condition | Behavior |
|-----------|----------|
| `[REDIS]` configured, Redis unreachable at startup | Log error, exit non-zero. No silent fallback to `.conf`. |
| Mid-flight Redis disconnect | Keep running on last-known in-memory config. Async context reconnects with exponential backoff (1 s → 30 s cap). On reconnect: re-subscribe to `config.changed`, trigger an internal `all` reload. |
| Bad pub/sub payload | Log and continue. The handler never propagates exceptions. |
| Live-state queue full | Drop oldest entry, increment `dropped_live_writes`, log once per minute. Audio is unaffected. |
| Sync PTY write fails (e.g. mute persistence) | Report error to the operator via PTY. In-memory state is not changed; the PTY command is rejected. |
| Dangling group reference (`user.group` → missing `group:*`) | User cannot authenticate. Warning logged at reload time. |

**Mid-flight outage summary:** the reflector continues to serve clients and
carry audio using the configuration snapshot taken at the last successful
Redis sync. New `config.changed` events during the outage are silently missed
(pub/sub is not buffered). On reconnect the internal `all` reload re-reads the
full current state, picking up any changes made while the connection was down.

---

## Monitoring

The `/status` JSON endpoint (enabled via `HTTP_SRV_PORT` in `[GLOBAL]`)
includes a `redis` object when `[REDIS]` is configured:

```json
{
  "nodes": { ... },
  "trunks": { ... },
  "redis": {
    "live_queue_size": 3,
    "dropped_live_writes": 0
  }
}
```

| Field | Description |
|-------|-------------|
| `live_queue_size` | Current number of pending live-state writes in the drain queue |
| `dropped_live_writes` | Cumulative count of live-state writes dropped due to queue overflow |

A non-zero `dropped_live_writes` indicates the drain timer is falling behind.
Possible causes: Redis latency spike, very high event rate, or a queue
capacity that is too small for the load. The audio path is never affected.

---

## Minimal working example

The following transcript sets up one user, mutes them on a trunk, adds a
cluster TG, and confirms live state. Assumes the reflector is running with
`[REDIS]` pointing at `127.0.0.1:6379`, no `KEY_PREFIX`, and at least one
trunk section named `TRUNK_1_2`.

```bash
# 1. Create a password group and a user
redis-cli HSET group:admins password "s3cur3p@ss"
redis-cli HSET user:ON4ABC group admins enabled 1
redis-cli PUBLISH config.changed users
# → (integer) 1

# 2. Verify the user can now authenticate
#    (connect a SvxLink node as ON4ABC with password "s3cur3p@ss" — it should succeed)

# 3. Mute ON4ABC on the trunk link
redis-cli SADD trunk:TRUNK_1_2:mutes ON4ABC
redis-cli PUBLISH config.changed trunk:TRUNK_1_2
# → (integer) 1

# 4. Confirm the mute is stored
redis-cli SMEMBERS trunk:TRUNK_1_2:mutes
# 1) "ON4ABC"

# 5. Add a cluster TG
redis-cli SADD cluster:tgs 9990
redis-cli PUBLISH config.changed cluster
# → (integer) 1

# 6. Start a transmission on TG 9990 from any connected node, then check
#    live state (within ~100 ms of TX start):
redis-cli HGETALL live:talker:9990
# 1) "callsign"
# 2) "SM0XYZ"
# 3) "started_at"
# 4) "1713200000"
# 5) "source"
# 6) "local"

# 7. Check /status for Redis metrics
curl -s http://localhost:8080/status | python3 -m json.tool | grep -A4 '"redis"'
# "redis": {
#     "live_queue_size": 0,
#     "dropped_live_writes": 0
# }

# 8. Unmute and clean up (optional)
redis-cli SREM trunk:TRUNK_1_2:mutes ON4ABC
redis-cli PUBLISH config.changed trunk:TRUNK_1_2
```
