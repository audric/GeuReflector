# MQTT Publishing

GeuReflector can publish real-time events and periodic status to an external
MQTT broker. This replaces the need to poll the HTTP `/status` endpoint —
dashboards and other consumers subscribe to MQTT topics and receive push
updates only when state changes.

## Quick start

Add an `[MQTT]` section to `svxreflector.conf`:

```ini
[MQTT]
HOST=mqtt.example.com
PORT=1883
USERNAME=reflector
PASSWORD=secret
TOPIC_PREFIX=svxreflector/myreflector
```

Restart the reflector. Events start flowing immediately.

Omit the `[MQTT]` section entirely to disable — there is zero overhead when
not configured.

---

## Configuration reference

| Field | Required | Default | Description |
|-------|----------|---------|-------------|
| `HOST` | Yes | — | MQTT broker hostname or IP |
| `PORT` | Yes | — | MQTT broker port (typically 1883, or 8883 for TLS) |
| `USERNAME` | Yes | — | Broker authentication username |
| `PASSWORD` | Yes | — | Broker authentication password |
| `TOPIC_PREFIX` | Yes | — | Base topic path (e.g. `svxreflector/myreflector`) |
| `STATUS_INTERVAL` | No | 30000 | Periodic full status publish interval in milliseconds. The full snapshot is a bootstrap/drift-correction mechanism — per-event topics already deliver every change live, so raising this is cheap. Lowering it below ~1 s is expensive once a mesh exceeds a few hundred nodes (retained payload is hundreds of KB). |
| `TLS_ENABLED` | No | 0 | Enable TLS encryption (`0` or `1`) |
| `TLS_CA_CERT` | If TLS | — | Path to CA certificate file |
| `TLS_CLIENT_CERT` | No | — | Path to client certificate (mutual TLS only) |
| `TLS_CLIENT_KEY` | No | — | Path to client private key (mutual TLS only) |

`TOPIC_PREFIX` has no default — set it to a unique identifier for this
reflector. It is not derived from `LOCAL_PREFIX` because a reflector may own
multiple prefixes while needing a single stable MQTT identity.

---

## Topic structure

All topics are published under the configured `TOPIC_PREFIX`. Subscribers can
use `TOPIC_PREFIX/#` to receive everything, or subscribe to specific subtrees.

### Talker events

Published when a client or trunk peer starts or stops talking on a talk group.

```
{TOPIC_PREFIX}/talker/{tg}/start
{TOPIC_PREFIX}/talker/{tg}/stop
```

Payload:
```json
{"callsign": "ON4ABC", "source": "local"}
```

`source` is `"local"` when the talker is a directly connected client, or
`"trunk"` when the talker is on a remote reflector reached via trunk.

### Client events

Published when a SvxLink node authenticates or disconnects.

```
{TOPIC_PREFIX}/client/{callsign}/connected
{TOPIC_PREFIX}/client/{callsign}/disconnected
```

Connected payload:
```json
{"tg": 1234, "ip": "192.168.1.10"}
```

Disconnected payload:
```json
{}
```

### Receiver (RX) status events

Published on every signal strength update from a connected node. Each node can
have multiple receivers — the payload contains all of them keyed by receiver ID.

```
{TOPIC_PREFIX}/client/{callsign}/rx
```

Payload:
```json
{"A":{"name":"Rx1","siglev":42,"enabled":true,"sql_open":true,"active":true},"B":{"name":"Rx2","siglev":0,"enabled":true,"sql_open":false,"active":false}}
```

| Field | Type | Description |
|-------|------|-------------|
| `name` | string | Receiver name from the node's QTH configuration |
| `siglev` | integer | Signal level (0–100) |
| `enabled` | boolean | Whether the receiver is enabled |
| `sql_open` | boolean | Whether the squelch is open |
| `active` | boolean | Whether the receiver is actively selected |

### Trunk events

Published when a trunk link's outbound or inbound connection goes up or down.

```
{TOPIC_PREFIX}/trunk/{section}/outbound/up
{TOPIC_PREFIX}/trunk/{section}/outbound/down
{TOPIC_PREFIX}/trunk/{section}/inbound/up
{TOPIC_PREFIX}/trunk/{section}/inbound/down
```

Up payload:
```json
{"host": "reflector-b.example.com", "port": 5302}
```

Down payload:
```json
{}
```

### Node lists

Published when the local node roster changes (debounced 500 ms after
client login / logout / TG change) and when a roster arrives from a
remote peer.

```
{TOPIC_PREFIX}/nodes/local           # this reflector's connected clients
{TOPIC_PREFIX}/nodes/<peer_id>       # roster mirrored from a trunk peer
                                     # or [TWIN] partner
```

Payload:
```json
{
  "timestamp": 1712345678,
  "nodes": [
    {"callsign": "ON4ABC", "tg": 2620, "qth_name": "Brussels",
     "lat": 50.85, "lon": 4.35},
    {"callsign": "IK1XYZ", "tg": 2221}
  ]
}
```

`lat`, `lon`, and `qth_name` are omitted when the node has not supplied
them. For trunk peers `<peer_id>` comes from the peer's `PEER_ID` (or
section name as fallback); for `[TWIN]` partners the id is `TWIN`.
Both topics are published with **retain** enabled so late-joining
subscribers see the last-known roster immediately.

### Periodic full status

Published at the configured `STATUS_INTERVAL` (default every 30 seconds).
The payload is identical to the HTTP `/status` JSON response.

```
{TOPIC_PREFIX}/status
```

This is the only topic published with MQTT **retain** enabled, so new
subscribers immediately receive the last known state.

---

## QoS and retain policy

| Topic | QoS | Retain |
|-------|-----|--------|
| `talker/...` | 0 | No |
| `client/.../connected` | 0 | No |
| `client/.../disconnected` | 0 | No |
| `client/.../rx` | 0 | No |
| `trunk/...` | 0 | No |
| `nodes/local` | 0 | **Yes** |
| `nodes/<peer_id>` | 0 | **Yes** |
| `status` | 0 | **Yes** |

QoS 0 (fire-and-forget) is used throughout. Event messages are ephemeral — a
missed talker start is covered by the next periodic status dump. Retain on
`status` ensures late-joining subscribers get a baseline immediately.

---

## TLS

To encrypt the connection to the broker, enable TLS:

```ini
[MQTT]
HOST=mqtt.example.com
PORT=8883
USERNAME=reflector
PASSWORD=secret
TOPIC_PREFIX=svxreflector/myreflector
TLS_ENABLED=1
TLS_CA_CERT=/etc/svxlink/mqtt-ca.crt
```

For mutual TLS (client certificate authentication), also set:

```ini
TLS_CLIENT_CERT=/etc/svxlink/mqtt-client.crt
TLS_CLIENT_KEY=/etc/svxlink/mqtt-client.key
```

The broker must be configured to listen on a TLS port and present a certificate
signed by the CA specified in `TLS_CA_CERT`.

---

## Error handling

MQTT publishing is best-effort and never impacts reflector operation:

- **Broker unreachable at startup:** Logged as a warning. The reflector starts
  normally and the MQTT client reconnects in the background (1s initial delay,
  30s max).
- **Broker disconnects:** Automatic reconnection. Messages published while
  disconnected are silently dropped.
- **Bad credentials:** Logged as an error. The client retries reconnection.
- **Missing required config fields:** Logged as an error. The reflector starts
  without MQTT.

---

## Implementation notes

The MQTT client uses [libmosquitto](https://mosquitto.org/) with
`mosquitto_loop_start()`, which runs a dedicated background thread for all
network I/O, reconnection, and keepalive. Publish calls from the reflector's
main event loop enqueue messages and return immediately — fully non-blocking.

### Dependencies

- **libmosquitto** — required at build time (`libmosquitto-dev`) and runtime
  (`libmosquitto1`)
- **jsoncpp** — already a project dependency, used for JSON payload
  serialization
- **OpenSSL** — already a project dependency, used by libmosquitto for TLS

### Docker

The Docker image includes libmosquitto. No extra setup is needed beyond adding
the `[MQTT]` section to your config file.

---

## Example: subscribing with mosquitto_sub

```bash
# Subscribe to all events from one reflector
mosquitto_sub -h mqtt.example.com -u user -P pass \
  -t 'svxreflector/myreflector/#' -v

# Subscribe to talker events only
mosquitto_sub -h mqtt.example.com -u user -P pass \
  -t 'svxreflector/myreflector/talker/#' -v

# Subscribe to trunk state across all reflectors
mosquitto_sub -h mqtt.example.com -u user -P pass \
  -t 'svxreflector/+/trunk/#' -v
```
