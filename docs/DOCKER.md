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

### 1. Prepare the config directory

```bash
mkdir config
cp /path/to/your/svxreflector.conf config/svxreflector.conf
```

The config file is mounted read-only into the container. Edit it on the host;
restart the container to apply changes (or use the PTY interface for live
user/password reloads — see below).

At minimum, add `LOCAL_PREFIX` and any `[TRUNK_x]` sections you need. See
[`docs/INSTALL.md`](INSTALL.md) for the config additions required for trunking.

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
