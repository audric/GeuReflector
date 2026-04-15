# Redis-backed Config Store

GeuReflector can optionally use a Redis server as the source of truth for user
credentials, password groups, cluster TGs, and per-trunk dynamic settings
(blacklists, allow-lists, TG maps). This lets a web dashboard push
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
- Trunk peer definitions (host, port, secret, remote_prefix, peer_id) — can be added/removed at runtime without restarting

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
| `trunk:<section>:peer` | HASH | `{ host, port, secret, remote_prefix, peer_id }` — runtime-addable trunk peer definition |
| `trunk:<section>:blacklist` | SET | TG patterns: exact (`666`), prefix (`24*`), range (`100-199`) |
| `trunk:<section>:allow` | SET | Same syntax as blacklist |
| `trunk:<section>:tgmap` | HASH | `{ peer_tg: local_tg }` decimal string pairs |

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
```

### Add a trunk peer at runtime

```bash
redis-cli HSET trunk:TRUNK_AB:peer \
    host reflector-b.example.com \
    port 5302 \
    secret shared_trunk_secret \
    remote_prefix 2 \
    peer_id my-peer-id
redis-cli PUBLISH config.changed trunk:TRUNK_AB
```

The reflector creates a new `TrunkLink` and starts the outbound handshake.

Fields:
- `host` (required)
- `port` (optional, default `5302`)
- `secret` (required) — the pre-shared trunk secret, identical on both ends
- `remote_prefix` (required) — comma-separated TG prefix(es) owned by the peer
- `peer_id` (optional, default: section name)

### Remove a trunk peer at runtime

```bash
redis-cli DEL trunk:TRUNK_AB:peer
redis-cli PUBLISH config.changed trunk:TRUNK_AB
```

The reflector tears down the TrunkLink cleanly, clearing any trunk
talker state it held. Only Redis-managed trunks can be removed this
way; peers defined statically in svxreflector.conf survive.

### Mute management (not in Redis)

Mutes are managed via the reflector's command PTY (`/dev/shm/reflector_ctrl`),
not through Redis. A dashboard issues mute commands by writing directly to
the PTY (e.g., `TRUNK MUTE TRUNK_1_2 ON4ABC`). Current mute state is exposed
in the `/status` JSON under each trunk's `muted` array — poll that endpoint
from the dashboard UI if you need to display current mute state.

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

## Static vs Redis-managed trunks

Trunk peers defined in `svxreflector.conf` as `[TRUNK_*]` sections are
treated as immutable at runtime — a `config.changed trunk:<section>`
event with no matching Redis peer hash is treated as a filter-reload
only (the existing behavior), never as a remove. Only trunks that were
added from Redis hashes can be dynamically destroyed.

---

## Failure modes

| Condition | Behavior |
|-----------|----------|
| `[REDIS]` configured, Redis unreachable at startup | Log error, exit non-zero. No silent fallback to `.conf`. |
| Mid-flight Redis disconnect | Keep running on last-known in-memory config. Async context reconnects with exponential backoff (1 s → 30 s cap). On reconnect: re-subscribe to `config.changed`, trigger an internal `all` reload. |
| Bad pub/sub payload | Log and continue. The handler never propagates exceptions. |
| Live-state queue full | Drop oldest entry, increment `dropped_live_writes`, log once per minute. Audio is unaffected. |
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

The following transcript sets up one user, adds a cluster TG, and confirms
live state. Assumes the reflector is running with `[REDIS]` pointing at
`127.0.0.1:6379`, no `KEY_PREFIX`, and at least one trunk section named
`TRUNK_1_2`.

```bash
# 1. Create a password group and a user
redis-cli HSET group:admins password "s3cur3p@ss"
redis-cli HSET user:ON4ABC group admins enabled 1
redis-cli PUBLISH config.changed users
# → (integer) 1

# 2. Verify the user can now authenticate
#    (connect a SvxLink node as ON4ABC with password "s3cur3p@ss" — it should succeed)

# 3. Add a cluster TG
redis-cli SADD cluster:tgs 9990
redis-cli PUBLISH config.changed cluster
# → (integer) 1

# 4. Start a transmission on TG 9990 from any connected node, then check
#    live state (within ~100 ms of TX start):
redis-cli HGETALL live:talker:9990
# 1) "callsign"
# 2) "SM0XYZ"
# 3) "started_at"
# 4) "1713200000"
# 5) "source"
# 6) "local"

# 5. Check /status for Redis metrics
curl -s http://localhost:8080/status | python3 -m json.tool | grep -A4 '"redis"'
# "redis": {
#     "live_queue_size": 0,
#     "dropped_live_writes": 0
# }
```

---

## PHP dashboard example

Examples below use the [**phpredis**](https://github.com/phpredis/phpredis)
PECL extension (the canonical `Redis` class). For pure-PHP setups without
the extension, [**Predis**](https://github.com/predis/predis) (installable
via Composer) exposes a nearly identical API — `new Predis\Client([...])`
in place of `new Redis()`, and the same `hSet` / `sAdd` / `publish` method
names work unchanged.

All snippets assume the reflector is configured with `KEY_PREFIX=refl1`.
Adjust `$prefix` for your deployment, or leave it empty when no prefix is
set on the reflector.

### Connect

```php
<?php
$r = new Redis();
$r->connect('127.0.0.1', 6379, 1.0);   // host, port, timeout seconds
// $r->auth('your_password');           // if PASSWORD= is set on the reflector
// $r->select(0);                       // if DB != 0

$prefix = 'refl1';                      // match the reflector's KEY_PREFIX
$k = fn(string $suffix) => $prefix === '' ? $suffix : "$prefix:$suffix";
$channel = $k('config.changed');
```

### Add a user

```php
$r->hSet($k('user:ON4ABC'),      'group',    'admins');
$r->hSet($k('user:ON4ABC'),      'enabled',  '1');
$r->hSet($k('group:admins'),     'password', 's3cur3p@ss');
$r->publish($channel, 'users');
```

Or atomic-ish with `MULTI/EXEC`:

```php
$r->multi()
  ->hSet($k('user:ON4ABC'),  'group',    'admins')
  ->hSet($k('user:ON4ABC'),  'enabled',  '1')
  ->hSet($k('group:admins'), 'password', 's3cur3p@ss')
  ->publish($channel, 'users')
  ->exec();
```

### List all users

```php
$users = [];
$it = null;
while (($keys = $r->scan($it, $k('user:*'), 200)) !== false) {
    foreach ($keys as $key) {
        $fields = $r->hGetAll($key);
        $callsign = substr($key, strlen($k('user:')));
        $users[$callsign] = $fields;
    }
}
// $users === ['ON4ABC' => ['group' => 'admins', 'enabled' => '1'], ...]
```

### Disable, re-enable, delete a user

```php
$r->hSet($k('user:ON4ABC'), 'enabled', '0');
$r->publish($channel, 'users');

$r->hSet($k('user:ON4ABC'), 'enabled', '1');
$r->publish($channel, 'users');

$r->del($k('user:ON4ABC'));
$r->publish($channel, 'users');
```

### Change a password-group's password

```php
$r->hSet($k('group:admins'), 'password', 'newS3cret');
$r->publish($channel, 'users');
```

### Add / remove a cluster TG

```php
$r->sAdd($k('cluster:tgs'), '9990');
$r->publish($channel, 'cluster');

$r->sRem($k('cluster:tgs'), '9990');
$r->publish($channel, 'cluster');
```

### Edit a per-trunk filter

```php
// Blacklist TG 666 on trunk TRUNK_1_2
$r->sAdd($k('trunk:TRUNK_1_2:blacklist'), '666');
$r->publish($channel, 'trunk:TRUNK_1_2');

// Remove the blacklist entry
$r->sRem($k('trunk:TRUNK_1_2:blacklist'), '666');
$r->publish($channel, 'trunk:TRUNK_1_2');

// Set a TG_MAP entry
$r->hSet($k('trunk:TRUNK_1_2:tgmap'), '7000', '1220');
$r->publish($channel, 'trunk:TRUNK_1_2');
```

### Add / remove a trunk peer at runtime

```php
// Add a trunk peer (link comes up without a reflector restart)
$r->hMSet($k('trunk:TRUNK_1_3:peer'), [
    'host'          => 'reflector-3.example.org',
    'port'          => '5302',
    'secret'        => 'shared_trunk_secret',
    'remote_prefix' => '3',
    'peer_id'       => 'my-peer-id',
]);
$r->publish($channel, 'trunk:TRUNK_1_3');

// Remove a trunk peer
$r->del($k('trunk:TRUNK_1_3:peer'));
$r->publish($channel, 'trunk:TRUNK_1_3');
```

### Read live state (connected clients, active talkers, trunk state)

```php
// Currently connected clients
$clients = [];
$it = null;
while (($keys = $r->scan($it, $k('live:client:*'), 200)) !== false) {
    foreach ($keys as $key) {
        $callsign = substr($key, strlen($k('live:client:')));
        $clients[$callsign] = $r->hGetAll($key);
    }
}

// Active talkers (one per TG)
$talkers = [];
$it = null;
while (($keys = $r->scan($it, $k('live:talker:*'), 200)) !== false) {
    foreach ($keys as $key) {
        $tg = (int)substr($key, strlen($k('live:talker:')));
        $talkers[$tg] = $r->hGetAll($key);
    }
}

// Trunk link state
$trunks = [];
$it = null;
while (($keys = $r->scan($it, $k('live:trunk:*'), 200)) !== false) {
    foreach ($keys as $key) {
        $section = substr($key, strlen($k('live:trunk:')));
        $trunks[$section] = $r->hGetAll($key);
    }
}
```

### Subscribe to live updates (push UI refresh)

The reflector publishes on `<KEY_PREFIX>:live.changed` once per drain
cycle (~75 ms). A dashboard worker / websocket backend can subscribe to
that channel and push refreshes to the browser instead of polling.

```php
$sub = new Redis();
$sub->connect('127.0.0.1', 6379);
// $sub->auth('...');
// $sub->select(0);
$sub->subscribe([$k('live.changed')], function ($redis, $chan, $msg) {
    // $msg is always "tick" — the signal is "something changed",
    // the dashboard re-reads live:* keys to get the new state.
    // Forward to your websocket / server-sent-events layer here.
    error_log("live.changed on $chan: $msg");
});
```

Subscribe is blocking; run it in a dedicated worker process.

### Running the importer from PHP

The `--import-conf-to-redis` CLI is idempotent — it's fine to invoke from
a dashboard bootstrap script when provisioning a new reflector:

```php
$output = [];
$rc = 0;
exec('svxreflector --config /etc/svxlink/svxreflector.conf '
   . '--import-conf-to-redis 2>&1', $output, $rc);
if ($rc !== 0) {
    throw new RuntimeException("import failed: " . implode("\n", $output));
}
```

### Error handling notes

- Every write op is idempotent — retrying after a transient network
  error is safe.
- The `publish` return value is the number of subscribers that received
  the message. `0` means the reflector isn't connected right now; the
  write still lands in Redis and will be picked up on the next reflector
  reconnect + full reload (see "Failure modes").
- `hSet` / `hMSet` create the hash if it doesn't exist; no need to
  pre-check.

---

## Migrating from a MySQL-backed setup

If you were previously running a custom dashboard that stored svxreflector
users and trunk definitions in MySQL, the path of least resistance is to
generate a `.conf` snippet from your MySQL tables, review it, and feed
it through the existing `--import-conf-to-redis` importer.

### Assumed MySQL schema

Every custom dashboard is different. The examples below assume tables
along these lines — adapt the queries to your actual column names:

```sql
CREATE TABLE users (
    callsign     VARCHAR(32)  PRIMARY KEY,
    group_name   VARCHAR(64)  NOT NULL,
    enabled      TINYINT(1)   NOT NULL DEFAULT 1
);

CREATE TABLE groups (
    name         VARCHAR(64)  PRIMARY KEY,
    password     VARCHAR(128) NOT NULL
);

CREATE TABLE cluster_tgs (
    tg           INT UNSIGNED PRIMARY KEY
);

CREATE TABLE trunk_peers (
    section         VARCHAR(64)  PRIMARY KEY,   -- e.g. "TRUNK_1_2"
    host            VARCHAR(255) NOT NULL,
    port            INT          NOT NULL DEFAULT 5302,
    secret          VARCHAR(128) NOT NULL,
    remote_prefix   VARCHAR(64)  NOT NULL,
    peer_id         VARCHAR(64)
);

CREATE TABLE trunk_filters (
    section         VARCHAR(64)  NOT NULL,
    kind            ENUM('blacklist','allow') NOT NULL,
    pattern         VARCHAR(32)  NOT NULL,      -- "666", "24*", "2427-2438"
    PRIMARY KEY (section, kind, pattern)
);

CREATE TABLE trunk_tgmap (
    section         VARCHAR(64)  NOT NULL,
    peer_tg         INT UNSIGNED NOT NULL,
    local_tg        INT UNSIGNED NOT NULL,
    PRIMARY KEY (section, peer_tg)
);
```

### MySQL → `.conf` → importer

Generate a `.conf`-style snippet from SQL, drop it alongside the real
reflector config, run the importer, then delete the snippet. No new
code lives on the dashboard side after the one-shot migration.

```php
<?php
// mysql-to-conf.php > /tmp/migration.conf
$db = new PDO(/* as above */);

echo "[USERS]\n";
foreach ($db->query('SELECT callsign, group_name FROM users WHERE enabled=1') as $row) {
    echo "{$row['callsign']}={$row['group_name']}\n";
}
echo "\n[PASSWORDS]\n";
foreach ($db->query('SELECT name, password FROM groups') as $row) {
    echo "{$row['name']}=\"{$row['password']}\"\n";
}

// CLUSTER_TGS goes under [GLOBAL]
echo "\n[GLOBAL]\nCLUSTER_TGS=";
$tgs = [];
foreach ($db->query('SELECT tg FROM cluster_tgs ORDER BY tg') as $row) {
    $tgs[] = $row['tg'];
}
echo implode(',', $tgs) . "\n";

// Trunk peers + filters
foreach ($db->query('SELECT * FROM trunk_peers') as $row) {
    echo "\n[{$row['section']}]\n";
    echo "HOST={$row['host']}\nPORT={$row['port']}\n";
    echo "SECRET={$row['secret']}\n";
    echo "REMOTE_PREFIX={$row['remote_prefix']}\n";
    if (!empty($row['peer_id'])) echo "PEER_ID={$row['peer_id']}\n";

    $filters = ['blacklist' => [], 'allow' => []];
    $stmt = $db->prepare('SELECT kind, pattern FROM trunk_filters WHERE section=?');
    $stmt->execute([$row['section']]);
    foreach ($stmt as $f) $filters[$f['kind']][] = $f['pattern'];
    if ($filters['blacklist']) echo "BLACKLIST_TGS=" . implode(',', $filters['blacklist']) . "\n";
    if ($filters['allow'])     echo "ALLOW_TGS="     . implode(',', $filters['allow'])     . "\n";

    $stmt = $db->prepare('SELECT peer_tg, local_tg FROM trunk_tgmap WHERE section=?');
    $stmt->execute([$row['section']]);
    $pairs = [];
    foreach ($stmt as $m) $pairs[] = "{$m['peer_tg']}:{$m['local_tg']}";
    if ($pairs) echo "TG_MAP=" . implode(',', $pairs) . "\n";
}
```

Run it, sanity-check, then:

```bash
php mysql-to-conf.php > /tmp/migration.conf
cat /etc/svxlink/svxreflector.conf /tmp/migration.conf > /tmp/merged.conf
# Eyeball /tmp/merged.conf, then:
svxreflector --config /tmp/merged.conf --import-conf-to-redis --dry-run
svxreflector --config /tmp/merged.conf --import-conf-to-redis
rm /tmp/migration.conf /tmp/merged.conf
```

The importer is idempotent, so re-running it does not produce duplicates.

Once the migration succeeds and the reflector is running against Redis,
consider **retiring MySQL** for reflector config — your dashboard can
read/write Redis directly (see the PHP examples above) and the reflector
picks up changes without a second hop.
