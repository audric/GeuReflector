# Installing GeuReflector as a replacement for SvxReflector

This guide is for sysops who already have a working SvxReflector installation
and want to replace it with GeuReflector. The binary name, config format, and
systemd service are all compatible — the transition is a drop-in replacement
plus a few config additions.

---

## Prerequisites

- A working SvxReflector installation (any recent version)
- Build tools: `git`, `cmake` (≥ 3.7), `g++` with C++11 support
- Development libraries (same as SvxReflector itself):
  ```bash
  # Debian/Ubuntu
  sudo apt install build-essential cmake libsigc++-2.0-dev libssl-dev \
                   libjsoncpp-dev libpopt-dev

  # Optional codec support
  sudo apt install libopus-dev libgsm1-dev libspeex-dev
  ```

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
LOCAL_PREFIX=1        # this reflector owns TGs starting with "1"
```

A comma-separated list is accepted if this reflector covers multiple prefix
groups:

```ini
LOCAL_PREFIX=11,12,13
```

### 6b. Add a trunk section for each peer

At the end of the config file, add one `[TRUNK_x]` section per peer reflector:

```ini
[TRUNK_1_2]
HOST=reflector-b.example.com
PORT=5302
SECRET=a_strong_shared_secret
REMOTE_PREFIX=2
```

- **The section name must be identical on both sides.** Both sysops must agree
  on a shared name (e.g. `TRUNK_1_2` for the link between prefix 1 and 2).
- `PORT` defaults to `5302` if omitted.
- Both sides must use the same `SECRET`.
- `REMOTE_PREFIX` also accepts a comma-separated list.

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
TRUNK_1_2: Connected to reflector-b.example.com:5302
TRUNK_1_2: Trunk hello from peer 'TRUNK_1_2' local_prefix=2 priority=3847291042
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

- [`docs/TRUNK_PROTOCOL.md`](TRUNK_PROTOCOL.md) — wire protocol specification
- [`docs/DEPLOYMENT_ITALY.md`](DEPLOYMENT_ITALY.md) — full national deployment
  example with configuration for all regions
