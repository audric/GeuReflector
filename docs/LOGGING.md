# Logging

GeuReflector has a leveled, per-subsystem, live-reloadable logger. This
document is for sysops who need to tune verbosity or diagnose a running
reflector without restarting it.

## Quick start

```ini
# In svxreflector.conf, under [GLOBAL]:
LOG=*=warn,trunk=debug,mqtt=info
```

Runtime control via the existing command PTY:

```bash
# Read current per-subsystem levels
echo "LOG SHOW"            > /dev/shm/reflector_ctrl

# Turn one subsystem verbose (and read back the ack)
echo "LOG trunk=debug"     > /dev/shm/reflector_ctrl

# Quiet everything
echo "LOG *=warn"          > /dev/shm/reflector_ctrl

# Back to whatever svxreflector.conf set at startup
echo "LOG RESET"           > /dev/shm/reflector_ctrl
```

Omit the `LOG=` tag entirely to keep today's behaviour — the default is
`*=info` for every subsystem.

---

## Levels

Five levels plus `off`. Each threshold admits itself and everything
noisier above it.

| Level    | Typical use                                                    |
| -------- | -------------------------------------------------------------- |
| `trace`  | Finest-grained dev diagnostics (not widely used)               |
| `debug`  | Diagnostic output for one subsystem — usually set temporarily  |
| `info`   | Narrative events (connections, talker changes, reloads)        |
| `warn`   | Non-fatal anomalies (reject bad config line, retry transient)  |
| `error`  | Faults that block a specific operation                         |
| `off`    | Suppress entirely                                              |

Line format on the wire:

```
[debug] [trunk] peer LU-MOBILE handshake nonce=0x4f3a...
```

When the reflector is started with `--logfile /var/log/svxreflector.log`
(or syslog), the existing `LogWriter` prepends a timestamp to each line
before writing:

```
Apr 18 14:23:05 [debug] [trunk] peer LU-MOBILE handshake nonce=0x4f3a...
```

---

## Subsystems

| Tag         | Covers                                                           |
| ----------- | ---------------------------------------------------------------- |
| `trunk`     | `TrunkLink` — peer handshake, talker arbitration, audio relay    |
| `satellite` | `SatelliteLink` / `SatelliteClient` — one-way mirror paths       |
| `twin`      | Twin-pair (HA) protocol — audio mirror, stale-inbound cleanup    |
| `client`    | Per-node TCP/UDP — auth, TLS handshake, RX, sendMsg              |
| `mqtt`      | MQTT publisher — connect, publish, subscribe, lifecycle          |
| `redis`     | Redis store / publisher — connect, read, write, lifecycle        |
| `core`      | Coordinator glue — startup, config, HTTP `/status`, PTY, TGHandler |

When a log line touches both a sink and a peer (e.g. publishing a
trunk-talker event to MQTT), it is tagged with the **sink** (`mqtt`)
— because that matches what a sysop saying "verbose for mqtt" wants to
see. Otherwise the tag matches the enclosing class.

An unknown subsystem name in `LOG=` or a `LOG sub=lvl` PTY command is
rejected (the PTY writes an error line; the config parser emits a
startup warning and ignores it).

---

## Configuration reference — `LOG=`

Syntax:

```
LOG=<sub>=<lvl>[,<sub>=<lvl>...]
```

- Comma-separated `<subsystem>=<level>` pairs.
- `*` sets the default for every subsystem **not** listed.
- If `LOG=` is absent or empty, the default is `*=info`.
- Whitespace around `=` and `,` is tolerated.

Examples:

```ini
# Quiet everything except trunk debug
LOG=*=warn,trunk=debug

# Follow MQTT publishing but keep the rest at info
LOG=mqtt=debug

# Silence the noisy redis subsystem entirely
LOG=redis=off

# Everything at warn — the most common production setting
LOG=*=warn
```

---

## PTY commands

All commands are sent to `/dev/shm/reflector_ctrl` (path set by
`COMMAND_PTY=` in `[GLOBAL]`; the default is already wired up in the
stock config). The PTY is half-duplex: write your command, then read
from the same file to get the response.

| Command                                 | Effect                                                     |
| --------------------------------------- | ---------------------------------------------------------- |
| `LOG SHOW`                              | Write the current `<sub>=<lvl>` snapshot back to the PTY   |
| `LOG <sub>=<lvl>`                       | Set one subsystem                                          |
| `LOG <sub1>=<lvl1>,<sub2>=<lvl2>`       | Set several at once, applied left-to-right                 |
| `LOG *=<lvl>`                           | Set the default for every subsystem                        |
| `LOG RESET`                             | Restore the levels read from `.conf` at startup            |

`LOG RESET` does **not** reread `.conf` from disk; it reverts in-memory
to the snapshot taken when the reflector started.

### Reading back the response

```bash
echo "LOG SHOW" > /dev/shm/reflector_ctrl
sleep 0.2
cat /dev/shm/reflector_ctrl
```

Expected output:

```
trunk=info
satellite=info
twin=info
client=info
mqtt=info
redis=info
core=info
```

---

## Docker deployments

Logs come out via the Docker `json-file` driver (10 MB × 3 files by
default in the SvxReflectorDashboard compose setup). Read them with:

```bash
docker compose logs -f svxreflector
```

To send a PTY command into a container:

```bash
docker compose exec svxreflector sh -c \
  'echo "LOG trunk=debug" > /dev/shm/reflector_ctrl'
```

The SvxReflectorDashboard web UI under `/admin/system_info` reads the
same Docker log stream via the Docker socket — it does not need any
special configuration.

---

## Non-Docker deployments (systemd / file logging)

When the reflector is started with `--logfile /var/log/svxreflector.log`,
the existing `LogWriter` redirects `stdout`/`stderr` to that file and
prefixes every line with a timestamp. `logrotate` integration works
unchanged — `SIGHUP` to the reflector causes `LogWriter` to reopen the
file.

Syslog is also supported: start with `--logfile syslog:`.

---

## Migrating from `TRUNK_DEBUG`

`TRUNK_DEBUG=1` in `[GLOBAL]` is **obsolete**. The replacement is:

```ini
LOG=trunk=debug
```

A startup warning fires if `TRUNK_DEBUG` is still present in `.conf`.
The new syntax gives strictly more control: you can now silence the
rest of the reflector while keeping trunk at debug, which the old
boolean flag could not do.

---

## Performance notes

- **Output is asynchronous.** Formatting happens on the calling thread,
  but the actual `write()` to `fd=1` is handed off to a dedicated
  background worker. The audio path never blocks on a log flush — a
  stalled log consumer (full pipe, paused `logrotate`, slow journald)
  does not stall audio forwarding.
- **Filtered lines cost one atomic load.** When a subsystem's level
  rules a line out, the call site checks one `std::atomic<int>` and
  returns without formatting arguments. Debug lines on per-frame paths
  cost nothing at runtime when the level is `warn`.
- **The worker never drops.** If the queue grows unboundedly (writer
  consumer is paused), the process accumulates memory rather than
  losing lines. In practice this does not happen; if you ever observe
  it, it is a sign of a genuinely stuck log consumer.
