# madpico

A [NomadNet](https://github.com/markqvist/NomadNet) compatible page node for the
**Raspberry Pi Pico 2 W**. It serves micron pages over the
[Reticulum](https://reticulum.network/) network straight from the
microcontroller: no Linux, no Python, just an RP2350 with 520 KB of RAM.

It is built on [microReticulum](https://github.com/attermann/microReticulum),
a C++ port of Reticulum, and the
[arduino-pico](https://github.com/earlephilhower/arduino-pico) core. Think of it
as a tiny [rns-page-node](https://github.com/Quad4-Software/rns-page-node) for
static pages.

## Features

- NomadNet compatible: announces as `nomadnetwork.node`, serves `/page/*.mu`
  to the NomadNet browser and other Python RNS clients
- Pages are stored in flash (LittleFS) and managed over USB with
  `tools/madpico.py`, without touching the node identity
- Built-in dynamic page `/page/status.mu` (uptime, memory, links, temperature)
- TCP client interface, wire compatible with rnsd's `TCPServerInterface`
  (connect to your own rnsd or a public transport node); optional UDP
  broadcast interface
- Persistent node identity, keys generated from the RP2350 hardware TRNG
- Automatic WiFi and TCP reconnect, fallback WiFi networks
- Link limit and idle timeout, so crawlers on public networks can't exhaust
  the RAM

## Measured performance

Pico 2 W connected via WiFi to a public transport node, client on the
internet using Python RNS:

| | |
|---|---|
| Link establishment | ~120-150 ms |
| Small page (single packet) | 20-45 ms |
| 1.2 KB page (resource, 3 parts) | 40-90 ms |
| 15 KB page (resource) | ~400 ms |
| Free heap with 16 active links | ~316 KB of 445 KB |
| Firmware size | ~590 KB flash, 79 KB static RAM |

Over a direct TCP connection in the LAN, 50 out of 50 requests succeeded with
an average of 20 ms for a small page.

## Hardware

- Raspberry Pi Pico 2 W (RP2350 + CYW43439). The Pico W (RP2040) is untested.
- 2.4 GHz WiFi only (CYW43439 limitation). The PCB antenna is weaker than a
  laptop's; if the Pico can't find your network, move it closer to the AP.

## Getting started

1. Install [PlatformIO Core](https://docs.platformio.org/en/latest/core/installation/index.html),
   e.g. `pipx install platformio`.
2. Create your local settings:
   ```sh
   cp src/secrets.h.example src/secrets.h
   $EDITOR src/secrets.h   # WiFi networks, TCP target, optional node name
   ```
3. Build: `pio run`
4. Flash:
   - first time: hold BOOTSEL while plugging in the Pico, then copy
     `.pio/build/pico2w/firmware.uf2` to the `RP2350` drive
   - afterwards: `tools/flash.sh` (uses the 1200 baud reset, no button needed)
5. Watch the log: `pio device monitor` or `tools/monitor.py --time 60`

On first boot the node creates its identity and two default pages
(`index.mu`, `about.mu`) and prints its address:

```
[rns] Node "madpico" running, address 0123456789abcdef0123456789abcdef
```

## Configuration

All options live in `src/secrets.h` (git-ignored); defaults are in
`src/config.h`.

| Option | Default | Description |
|---|---|---|
| `WIFI_NETWORKS` | - | `{ {"ssid", "password"}, ... }`, tried in order |
| `TCP_TARGET_HOST` / `TCP_TARGET_PORT` | - / 4242 | TCPServerInterface to connect to |
| `NODE_NAME` | `madpico` | name shown in NomadNet |
| `USE_TCP` / `USE_UDP` | 1 / 0 | enable interfaces |
| `UDP_PORT` | 4242 | UDP broadcast port |
| `ANNOUNCE_INTERVAL_MS` | 6 h | periodic announce (plus one after every TCP connect) |
| `NTP_SERVER_1` / `NTP_SERVER_2` | `pool.ntp.org` / `time.cloudflare.com` | time source (required, see below) |
| `STATUS_SHOW_NETWORK` | 0 | show SSID and local IP on the status page |
| `MAX_PAGE_SIZE` | 16 KB | largest page served / accepted for upload |
| `MAX_ACTIVE_LINKS` | 16 | links beyond this close the longest idle ones |
| `LINK_IDLE_TIMEOUT_S` | 300 | close links without data (keepalives don't count) |
| `WATCHDOG_TIMEOUT_MS` | 8000 | reboot if the main loop hangs |

### rnsd side

To let the Pico connect to your own rnsd, add a TCP server interface to
`~/.reticulum/config`:

```ini
  [[madpico TCP Server]]
    type = TCPServerInterface
    enabled = yes
    listen_ip = 0.0.0.0
    listen_port = 4242
```

Alternatively point `TCP_TARGET_HOST` at a public transport node to make the
node reachable from the whole network.

For the optional UDP interface (`USE_UDP 1`), use a matching `UDPInterface`
with `forward_ip` set to your subnet's broadcast address. Note that WiFi
broadcasts are not acknowledged on the radio layer, so occasional packet loss
is expected; TCP is more reliable.

## Managing pages

`tools/madpico.py` talks to the node over USB serial (requires `pyserial`).
Paths are relative to the pages directory, so `sub/page.mu` is served as
`/page/sub/page.mu`. Changes take effect immediately, no reboot needed.

```sh
tools/madpico.py ls                        # list pages with size and CRC
tools/madpico.py get index.mu              # download
tools/madpico.py put mypage.mu             # upload as /page/mypage.mu
tools/madpico.py put local.mu sub/x.mu     # upload as /page/sub/x.mu
tools/madpico.py rm mypage.mu              # delete
tools/madpico.py sync ./pages --delete     # mirror a directory (only changed files)
tools/madpico.py df                        # flash usage
```

Use `--port` or `$MADPICO_PORT` if the Pico is not `/dev/ttyACM0`.
Transfers are verified with CRC-32. `/page/status.mu` is built in and can't be
replaced.

> Don't use `pio run -t uploadfs`: it overwrites the whole filesystem,
> including the node identity, which gives the node a new address.

### Serial console

Single characters typed into the serial console:

| Key | Action |
|---|---|
| `a` | announce now (use sparingly, see below) |
| `s` | print status (heap, links, connections) |
| `w` | scan WiFi networks |
| `c` | reconnect WiFi |
| `r` | rewrite the default pages and reboot |

LED: on = online, slow blink = WiFi ok but TCP disconnected, fast blink = no WiFi.

### Announces and rate limits

Transport nodes rate-limit announces per destination (typically about one
per hour). A node that announces too often has its announces silently
dropped for a while: they are no longer rebroadcast, so it doesn't show up
in other people's NomadNet announce lists. Path requests still work, so the
node stays reachable by address (`<address>:/page/index.mu`).

madpico announces every 6 hours and after every TCP (re)connect. The
announce after a reconnect is required: a transport node forgets the path
when the old connection goes away. Even when the node is rate-limited, the
announce still updates the transport node's routing data, it just isn't
rebroadcast. Note that a transport node does not create a *new* path entry
from a rate-limited announce, so many reboots in a short time (e.g. while
developing) can make the node unreachable until the penalty expires. Avoid
pressing `a` repeatedly.

## Testing

`tools/fetch_page.py` fetches pages with Python RNS, using a separate
Reticulum config directory so your own setup is not affected:

```sh
tools/fetch_page.py --config /path/to/test-config --hash <node address> \
    --path /page/index.mu --path /page/status.mu
```

## Implementation notes

Things found while porting, which may be useful for others using
microReticulum on the RP2350:

- **RNG seeding:** the Crypto library has no hardware entropy source for
  RP2xxx, so the RNG starts from the same state on every boot and every
  device would generate the same keys. `src/PicoEntropy.h` feeds the RP2350
  TRNG into the RNG as a noise source.
- **Clock:** without an RTC, microReticulum's time is "milliseconds since
  boot + persisted offset", i.e. 1970, and it jumps back by up to 10 minutes
  after a reboot because the offset is only persisted every 10 minutes.
  Announces carry this time as emission timestamp, and Python RNS only
  accepts an announce for a known destination if it is newer than the last
  one, so announces after a reboot were silently dropped. madpico sets the
  clock from NTP before the first announce.
- **Global `RNS::Reticulum`:** its default constructor logs via `Serial`.
  Created as a global object it runs before arduino-pico has initialised USB
  and the board hangs before `setup()`. It is created in `setup()` instead.
- **Persistence without transport mode:** the flash-backed path, identity and
  packet hash stores are only initialised (and capped) when transport mode is
  enabled. On a plain endpoint every insert fails, including duplicate
  detection. madpico keeps these tables in RAM (`RNS_PERSIST_*=0`) and sets
  the caps itself.
- **Link MTU:** over TCP, Python RNS signals a link MTU of 262144.
  microReticulum stores it as `uint16_t`, which happens to truncate to 0 and
  fall back to 500, so it works, but by accident.
- **Log level:** DEBUG/TRACE logs print private keys; the build caps the log
  level at VERBOSE so they aren't compiled in.
- **arduino-pico WiFi:** calling `WiFi.mode()` makes `WiFi.begin()` return
  immediately instead of waiting for the connection. The first join right
  after a cold boot also fails regularly, so startup retries several rounds.
- **LittleFS adapter:** microStore's LittleFS adapter targets the ESP32 API;
  `src/PicoLittleFS.h` implements it for arduino-pico.

## Limitations

- Static pages only (plus built-in dynamic pages in C++); no executable
  scripts like NomadNet or rns-page-node
- Pages up to 16 KB, no bz2 compression for resources
- No file downloads (`/file/...`) and no LXMF messaging
- Not a transport node

## License

Apache License 2.0, see [LICENSE](LICENSE).

The request handling is based on the nomadnet example of
[microReticulum](https://github.com/attermann/microReticulum) by Chad
Attermann (Apache-2.0). Dependencies (fetched by PlatformIO, not included):
microReticulum and microStore (Apache-2.0), Crypto (MIT), ArduinoJson (MIT),
MsgPack (MIT), arduino-pico (LGPL-2.1).
