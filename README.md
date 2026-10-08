# DMX Bridge — ESP32-S3 Art-Net / sACN → DMX512

Wireless DMX node for QLC+ (or any Art-Net / E1.31 controller). The ESP32-S3 joins your Wi-Fi,
receives one universe of Art-Net or sACN and sends it out as DMX512 through an RS485 transceiver.

## Wiring (Elecrow UART TTL ↔ RS485 auto-direction module)

| ESP32-S3        | RS485 module | DMX XLR      |
|-----------------|--------------|--------------|
| GPIO17 (TX)     | RXD          |              |
| (unused)        | TXD          |              |
| 5V (VBUS/VIN)   | VCC          |              |
| GND             | GND          | pin 1 (GND)  |
|                 | A            | pin 3 (D+)   |
|                 | B            | pin 2 (D−)   |

* The module switches direction by itself, so there is no DE pin (`DE GPIO = -1`). If you use a
  plain MAX485 board instead, wire DE+RE together to a GPIO and set it in the web UI.
* **Power:** 5 V gives the full RS485 swing (~2–3 V between A and B); 3.3 V also works with less
  margin on long runs. The ESP's 3.3 V TX is a valid "high" for a 5 V MAX485. At 5 V, never
  connect the module's TXD to the ESP: ESP32 GPIOs are not 5 V tolerant. If the module's RXD
  measures ~5 V with the ESP disconnected (pull-up to VCC), power the module from 3V3 instead.
* TX → RXD is the usual UART crossover: the module's RXD is its input, which it drives onto A/B.
  The firmware only transmits, so the module's TXD (its output) can stay unconnected.
* An auto-direction module drives "0" bits hard but "1" bits only through its bias resistors. Put
  a 120 Ω terminator on the last fixture. If long cable runs are still unreliable, use a
  DE-controlled MAX485/MAX3485 board.
* Don't use GPIO19/20 (USB) or 22–32 (flash/PSRAM); the web UI rejects them.

## Build & flash

Built with ESP-IDF v6.2 for a 16 MB flash ESP32-S3 (2 × 4 MB OTA app slots + 8 MB spare data partition) (`~/esp/esp-idf`):

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3        # once
idf.py build
idf.py -p /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_68:B6:B3:47:FF:1C-if00 flash monitor
```

Default pins, UART, AP password and hostname prefix are under `idf.py menuconfig` → *DMX Bridge*.
All of them except the AP password can also be changed in the web UI.

## First-time setup (Wi-Fi provisioning)

1. With no Wi-Fi stored, the bridge opens an access point **`DMX-Bridge-XXXX`**
   (password `dmxbridge`). XXXX is the end of its MAC address.
2. Join it with a phone or laptop. The captive portal opens; if it doesn't, browse to
   `http://192.168.4.1`.
3. Click *Scan networks*, pick your SSID, enter the password and click **Save**. The bridge
   restarts and joins your Wi-Fi.
4. Find it at **`http://dmx-bridge-XXXX.local`**, or check your router's DHCP list.

If the Wi-Fi is unreachable for 20 s the AP comes back, so you can always reconfigure it. It keeps
retrying your network in the background and closes the AP once it is connected and nobody is on
the AP.

**Over USB instead:** connect a serial terminal (`idf.py -p <port> monitor`, or
`picocom /dev/ttyACM*`) and type `wifi "My Network" mypassword`. The bridge saves it and
restarts. See [Serial console](#serial-console).

**Factory reset:** hold BOOT (GPIO0) for 5 s, or use the button in the web UI.

## Discovery

* **mDNS:** `dmx-bridge-XXXX.local`, with services `_http._tcp` (web UI), `_artnet._udp` and
  `_sacn._udp`. The TXT records carry `universe`, `proto`, `fw` and `mac`.
  `avahi-browse -rt _artnet._udp` lists them.
* **Art-Net:** answers ArtPoll, so it appears in QLC+'s node list and in any Art-Net tool.

## QLC+ setup

**Art-Net**
1. *Inputs/Outputs* → pick a universe → enable **Art-Net** output on your Wi-Fi/LAN interface.
2. Open its settings (wrench icon): set *IP address* to the bridge's IP (unicast, best over
   Wi-Fi) or leave it on broadcast, and set *Universe* to the bridge's Art-Net universe
   (default **0**).

**sACN / E1.31**
1. Enable **E1.31** output on that interface.
2. Settings: *Universe* = bridge sACN universe (default **1**). Use *Multicast* or *Unicast* to
   the bridge's IP. Unicast is usually more reliable on Wi-Fi, because many access points
   handle multicast badly.

Both protocols are accepted by default. If both arrive at once, the higher sACN priority wins;
Art-Net counts as priority 100.

## Web UI

* **Status:** Wi-Fi, IP, active source, packets/s, DMX frames/s and a live view of all 512
  output channels.
* **DMX input:** protocol, Art-Net universe (shown as Net/Sub/Uni), sACN universe, hold or
  blackout on signal loss, loss timeout, refresh rate (default 40 Hz; a full 512-slot frame
  allows about 44 Hz).
* **Hardware:** TX/DE/LED GPIOs, UART. Pin, Wi-Fi and hostname changes restart the bridge.

* **Firmware:** upload a new `build/dmx_bridge.bin` from the browser. It is written to the
  inactive OTA slot, verified, and booted. If the new firmware crashes before it finishes
  starting, the bootloader rolls back to the previous one on the next reset.

JSON API: `GET /api/status`, `GET/POST /api/config`, `GET /api/scan`, `POST /api/reboot`,
`POST /api/factory_reset`,
`POST /api/ota` (raw `.bin` as the body, e.g.
`curl --data-binary @build/dmx_bridge.bin http://dmx-bridge-XXXX.local/api/ota`).

## Serial console

The USB-Serial/JTAG port carries the log and a command line (`dmx>` prompt):

| Command | |
|---|---|
| `help` | list commands |
| `status` | Wi-Fi state, IP, active DMX source, packet counters |
| `wifi <ssid> [password]` | save Wi-Fi credentials and restart (quote SSIDs with spaces) |
| `wifi --forget` | erase credentials, restart into setup-AP mode |
| `scan` | list nearby Wi-Fi networks |
| `config` | show all settings |
| `set <key> <value>` | change a setting, e.g. `set sacn_universe 2`, `set protocol artnet`, `set on_loss blackout` |
| `dmx [n]` | show the first n output channels (default 32) |
| `log <level> [tag]` | change log verbosity, e.g. `log warn` to quiet the console |
| `reboot` / `factory_reset yes` | restart / erase all settings |

`set` accepts the same keys and rules as the web UI. Pin, UART and hostname changes take effect
after `reboot`.

## Testing without QLC+

```bash
tools/send_test.py poll                          # discover Art-Net nodes
tools/send_test.py artnet <bridge-ip>            # Art-Net chase on universe 0
tools/send_test.py sacn <bridge-ip> --pattern sine
tools/send_test.py sacn --universe 1             # multicast
```

## DMX timing

250 kbit/s 8N2; break 176 µs, mark-after-break 12 µs, start code 0 + 512 slots, sent
continuously by a task pinned to core 1. Wi-Fi power save is disabled to keep latency low.
