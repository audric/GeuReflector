# Installing GeuReflector as a replacement for SvxReflector

This guide is for sysops who already have a working SvxReflector installation
and want to replace it with GeuReflector. The binary name, config format, and
systemd service are all compatible — the transition is a drop-in replacement
plus a few config additions.

---

## Prerequisites

- A working SvxReflector installation (any recent version)
- Build tools: `git`, `cmake` (≥ 3.7), `g++` with C++11 support
- Development libraries:
  ```bash
  # Debian/Ubuntu — required
  sudo apt install build-essential cmake libsigc++-2.0-dev libssl-dev \
                   libjsoncpp-dev libpopt-dev

  # Required for MQTT publishing (skip if building with -DWITH_MQTT=OFF)
  sudo apt install libmosquitto-dev

  # Required for Redis-backed config store (skip if building with -DWITH_REDIS=OFF)
  sudo apt install libhiredis-dev

  # Optional codec support
  sudo apt install libopus-dev libgsm1-dev libspeex-dev
  ```

The MQTT and Redis backends are enabled by default. If your deployment doesn't
need them, see step 2 below for the opt-out CMake flags — that lets you skip
the matching `*-dev` packages entirely.

---

## 1. Get the source

```bash
git clone https://github.com/IW1GEU/geureflector.git
cd geureflector
```

---

## 2. Build

```bash
cmake -S src -B build -DLOCAL_STATE_DIR=/var
cmake --build build
```

The compiled binary is at `build/bin/svxreflector`.

### Optional: build without MQTT or Redis

The MQTT publisher and Redis-backed config store are enabled by default. To
build without them — for example, on a host where you don't want to install
`libmosquitto-dev` or `libhiredis-dev` — pass the matching CMake flags:

```bash
# build without MQTT support
cmake -S src -B build -DLOCAL_STATE_DIR=/var -DWITH_MQTT=OFF

# build without Redis support
cmake -S src -B build -DLOCAL_STATE_DIR=/var -DWITH_REDIS=OFF

# build without either
cmake -S src -B build -DLOCAL_STATE_DIR=/var -DWITH_MQTT=OFF -DWITH_REDIS=OFF
```

CMake prints which mode it picked (`-- WITH_MQTT=ON: building with libmosquitto
support`) at configure time.

Behavior with the feature disabled:

- **`WITH_MQTT=OFF`** + `[MQTT]` section in your conf → reflector logs a
  warning and continues without publishing. Safe to leave `[MQTT]` in place.
- **`WITH_REDIS=OFF`** + `[REDIS]` section in your conf → reflector aborts
  startup (since Redis-mode auth would not work). Remove the `[REDIS]` section
  before starting.

### Optional: change where the binary looks for its config

By default the binary is built to look for its config under
`${CMAKE_INSTALL_PREFIX}/etc/svxlink/svxreflector.conf` (typically
`/usr/local/etc/svxlink/svxreflector.conf`). To make it look under
`/etc/svxlink/` instead — the usual Debian/Ubuntu location — add
`-DCMAKE_INSTALL_SYSCONFDIR=/etc` at configure time:

```bash
cmake -S src -B build -DLOCAL_STATE_DIR=/var -DCMAKE_INSTALL_SYSCONFDIR=/etc
cmake --build build
```

Any absolute path works; `/svxlink` is appended automatically. For example
`-DCMAKE_INSTALL_SYSCONFDIR=/opt/myreflector` makes the binary read
`/opt/myreflector/svxlink/svxreflector.conf`.

You can also override the location at runtime, without rebuilding, via the
`--config` flag:

```bash
svxreflector --config /path/to/svxreflector.conf
```

For a systemd-managed install, add the flag to the unit's `ExecStart=` line.

The full lookup order at startup is: `--config` argument → 
`~/.svxlink/svxreflector.conf` → the compiled-in path above.

---

## 3. Stop the running service

```bash
sudo systemctl stop svxreflector
```

---

## 4. Back up the original binary

Find where your current binary lives:

```bash
which svxreflector
# typically /usr/bin/svxreflector or /usr/local/bin/svxreflector
```

Back it up:

```bash
sudo cp /usr/bin/svxreflector /usr/bin/svxreflector.orig
```

---

## 5. Install the new binary

```bash
sudo cp build/bin/svxreflector /usr/bin/svxreflector
```

Use the same destination path you found in step 4.

---

## 6. Update the configuration

Your existing `/etc/svxlink/svxreflector.conf` works as-is — GeuReflector is
fully backwards compatible. The trunk features are opt-in: if you add nothing,
the reflector behaves exactly like the original.

To enable trunking, add two things:

### 6a. Declare this reflector's TG prefix

In the `[GLOBAL]` section:

```ini
[GLOBAL]
# ... existing settings ...
LOCAL_PREFIX=262      # this reflector owns TGs starting with "262"
                      # Use your country's Mobile Country Code (MCC):
                      # e.g. 222 = Italy, 240 = Sweden, 262 = Germany.
                      # Anchoring on the MCC keeps your prefix space
                      # from colliding with other countries' reflectors.
                      # Full table: docs/MCC_COUNTRY_CODES.md
```

A comma-separated list is accepted if this reflector covers multiple prefix
groups (e.g. a country that holds more than one MCC):

```ini
LOCAL_PREFIX=234,235  # United Kingdom (two MCC blocks)
```

### 6b. Add a trunk section for each peer

At the end of the config file, add one `[TRUNK_x]` section per peer reflector:

```ini
[TRUNK_DE_SE]
HOST=reflector-se.example.com
PORT=5302
SECRET=a_strong_shared_secret
REMOTE_PREFIX=240
```

- **The section name must be identical on both sides.** Both sysops must agree
  on a shared name. For international links the standard is
  `TRUNK_<ISO>_<ISO>` using each country's ISO 3166-1 alpha-2 code, with the two
  codes in **alphabetical order** — e.g. `TRUNK_DE_SE` for Germany (`262`) and
  Sweden (`240`). Sorting alphabetically makes the name deterministic, so both
  sysops reach the same name independently with no "my country first" dispute.
- `PORT` defaults to `5302` if omitted.
- Both sides must use the same `SECRET`.
- `REMOTE_PREFIX` also accepts a comma-separated list.

For fuller examples, see [`docs/DEPLOYMENT_ITALY.md`](DEPLOYMENT_ITALY.md)
(20-region national mesh) and [`docs/WW_DEPLOYMENT.md`](WW_DEPLOYMENT.md)
(25-country worldwide mesh).

### 6c. Optional: enable the HTTP status endpoint

```ini
[GLOBAL]
# ... existing settings ...
HTTP_SRV_PORT=8080
```

The `/status` endpoint will include a `trunks` object showing connection state
and active talkers per link.

---

## 7. Open the trunk port in the firewall

The trunk uses TCP port `5302` (separate from the client port `5300`).

```bash
# firewalld
sudo firewall-cmd --permanent --add-port=5302/tcp
sudo firewall-cmd --reload

# ufw
sudo ufw allow 5302/tcp

# iptables
sudo iptables -A INPUT -p tcp --dport 5302 -j ACCEPT
```

---

## 8. Start the service

```bash
sudo systemctl start svxreflector
sudo systemctl status svxreflector
```

---

## 9. Verify the trunk is working

Check the logs for the handshake message:

```bash
journalctl -u svxreflector -f
```

A successful trunk connection looks like:

```
TRUNK_DE_SE: Connected to reflector-se.example.com:5302
TRUNK_DE_SE: Trunk hello from peer 'TRUNK_DE_SE' local_prefix=240 priority=3847291042
```

If `HTTP_SRV_PORT` is set, query the status endpoint:

```bash
curl -s http://localhost:8080/status | python3 -m json.tool
```

The `trunks` object will show `"connected": true` for each active link.

---

## Rolling back

If anything goes wrong, restore the original binary and restart:

```bash
sudo systemctl stop svxreflector
sudo cp /usr/bin/svxreflector.orig /usr/bin/svxreflector
sudo systemctl start svxreflector
```

No config changes are needed to roll back — the original binary simply ignores
the `LOCAL_PREFIX` and `[TRUNK_x]` entries.

---

## Further reading

- [`docs/PEER_PROTOCOL.md`](PEER_PROTOCOL.md) — wire protocol specification
- [`docs/DEPLOYMENT_ITALY.md`](DEPLOYMENT_ITALY.md) — full national deployment
  example with configuration for all regions
