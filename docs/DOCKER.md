# Running GeuReflector in Docker

This guide covers building a Docker image and running the reflector as a
container. The image uses a two-stage build: a full build environment compiles
the binary, then only the runtime libraries and the binary are copied into a
minimal `debian:bookworm-slim` image.

---

## Prerequisites

- Docker Engine ≥ 20.10
- Docker Compose v2 (the `docker compose` plugin, not the legacy `docker-compose`)

---

## Quick start

### 1. Create the config file *first*

`docker-compose.yml` bind-mounts `./config/svxreflector.conf` into the container
as a file. **You must create that file before the first `docker compose up`.**

> **Gotcha:** if `./config/svxreflector.conf` does not exist when the container
> starts, Docker silently creates it as an **empty directory**, and the
> reflector then fails to start. If that happens, stop the stack, delete the
> stray directory (`rm -rf config/svxreflector.conf`), create the file as below,
> and start again.

The repository does not ship a ready-to-run config — it ships the template
`src/svxlink/reflector/svxreflector.conf.in`. Start from that:

```bash
mkdir -p config
cp src/svxlink/reflector/svxreflector.conf.in config/svxreflector.conf
# then edit config/svxreflector.conf
```

At minimum set `LISTEN_PORT`, set `[SERVER_CERT] COMMON_NAME` (**required** — the
reflector aborts at startup without it, and with `restart: unless-stopped` the
container then crash-loops), add a `[USERS]`/`[PASSWORDS]` entry for your node,
and (for trunking) `LOCAL_PREFIX` plus the `[TRUNK_x]` sections. The reflector
auto-generates its CA and server certificate into the `pki` volume on first run.
See the [README quick start](../README.md#quick-start--one-standalone-reflector)
for a minimal standalone config and [`docs/INSTALL.md`](INSTALL.md) for the
trunking additions.

The file is mounted read-only; edit it on the host and restart the container to
apply changes (or use the PTY interface for live user/password reloads — see
below).

### 2. Build and start

```bash
docker compose up -d
```

This builds the image on first run. To rebuild after a source change:

```bash
docker compose up -d --build
```

### 3. Check logs

```bash
docker compose logs -f
```

A healthy trunk connection appears as:

```
TRUNK_2: Connected to reflector-b.example.com:5302
TRUNK_2: Trunk hello from peer 'TRUNK_1' local_prefix=2 priority=3847291042
```

---

## Ports

| Port       | Protocol | Purpose                                      |
|------------|----------|----------------------------------------------|
| 5300       | TCP      | SvxLink client connections                   |
| 5300       | UDP      | SvxLink client audio                         |
| 5302       | TCP      | Server-to-server trunk links                 |
| 5303       | TCP      | Satellite connections (optional)             |
| 8080       | TCP      | HTTP `/status` endpoint (optional)                 |

If you do not set `HTTP_SRV_PORT` in the config, remove the `8080` port mapping
from `docker-compose.yml`.

---

## Volumes

| Mount point                          | Purpose                                         |
|--------------------------------------|-------------------------------------------------|
| `/etc/svxlink/svxreflector.conf`     | Configuration file (mount from host, read-only) |
| `/etc/svxlink/pki/`                  | TLS certificates and CA state (persistent)      |

The `pki` volume persists certificates across container restarts. If you manage
certificates externally, you can remove this volume and mount your cert directory
directly instead.

---

## Live user/password reload (PTY interface)

The command PTY is available inside the container at `/dev/shm/reflector_ctrl`
(set by `COMMAND_PTY` in the config). Use `docker exec` to write to it:

```bash
docker compose exec svxreflector \
  sh -c 'echo "CFG PASSWORDS MyNodes newpassword" > /dev/shm/reflector_ctrl'
```

---

## Building the image manually (without Compose)

```bash
docker build -t geureflector .

docker run -d \
  --name svxreflector \
  --restart unless-stopped \
  -p 5300:5300/tcp \
  -p 5300:5300/udp \
  -p 5302:5302/tcp \
  -p 8080:8080/tcp \
  -v ./config/svxreflector.conf:/etc/svxlink/svxreflector.conf:ro \
  -v svxreflector_pki:/etc/svxlink/pki \
  geureflector
```

---

## Passing additional flags

The container entrypoint is `svxreflector`. Append any supported flags after
the service definition or in the `command:` key of `docker-compose.yml`:

```yaml
command: ["--config", "/etc/svxlink/svxreflector.conf", "--logfile", "/var/log/svxreflector.log"]
```

Available flags:

| Flag | Description |
|------|-------------|
| `--config <file>` | Config file path (default used by the image: `/etc/svxlink/svxreflector.conf`) |
| `--logfile <file>` | Redirect stdout/stderr to a file instead of the container log |
| `--runasuser <user>` | Drop privileges to the given user after startup |
| `--pidfile <file>` | Write PID to file (not normally needed in containers) |

Do **not** pass `--daemon` — Docker manages the process lifecycle.

---

## Updating

To update to a new version of the source:

```bash
git pull
docker compose up -d --build
```

The old container is replaced; the `pki` volume is preserved.
