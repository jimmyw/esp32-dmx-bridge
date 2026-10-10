# DMX Bridge — ESP32-S3 Art-Net / sACN → DMX512

Wireless DMX node for QLC+ (or any Art-Net / E1.31 controller). The ESP32-S3 joins your Wi-Fi,
receives one universe of Art-Net or sACN and sends it out as DMX512 through an RS485 transceiver.

## Wiring (Elecrow UART TTL ↔ RS485 auto-direction module)

| ESP32-S3        | RS485 module | DMX XLR      |
|-----------------|--------------|--------------|
| GPIO17 (TX)     | TXD          |              |
| (unused)        | RXD          |              |
| 5V (VBUS/VIN)   | VCC          |              |
| GND             | GND          | pin 1 (GND)  |
|                 | A            | pin 3 (D+)   |
|                 | B            | pin 2 (D−)   |

* The module switches direction by itself, so there is no DE pin (`DE GPIO = -1`). If you use a
  plain MAX485 board instead, wire DE+RE together to a GPIO and set it in the web UI.
* **Power:** 5 V gives the full RS485 swing (~2–3 V between A and B); 3.3 V also works with less
  margin on long runs. The ESP's 3.3 V TX is a valid "high" for a 5 V MAX485. At 5 V, never
  connect the module's RXD (its output) to the ESP: ESP32 GPIOs are not 5 V tolerant. If the
  module's TXD input measures ~5 V with the ESP disconnected (pull-up to VCC), power the module
  from 3V3 instead.
* This module labels its pins from the MCU's side, so **ESP TX goes to the module's TXD**. That's
  the input it drives onto A/B, confirmed on a scope. RXD is the module's output back to the MCU.
  The firmware only transmits, so RXD stays unconnected.
* **Bias resistors are required with this module (see below).**
* Don't use GPIO19/20 (USB) or 22–32 (flash/PSRAM); the web UI rejects them.

### Bias resistors and termination

The auto-direction module only drives the line for "0" bits. For "1" bits (and idle) it drives a
short pulse, then switches its driver off, and the bus would float at ~0 V between A and B. The
board has no fail-safe bias resistors of its own. A DMX receiver needs **A−B ≥ +0.2 V** to read
a "1", so add two resistors on the module's RS485 side:

```
  VCC (5 V) ──[ 680 Ω ]──┬── A (XLR pin 3) ════ cable ════╗
                         │                               ║  last fixture:
                         │                             [120 Ω] termination
                         │                               ║  (A–B, far end only)
  GND ───────[ 680 Ω ]──┴── B (XLR pin 2) ════ cable ════╝
```

* Pull-up **680 Ω from A to the module's 5 V VCC**, pull-down **680 Ω from B to GND**.
  Use the real GND, the one on the TTL side that is shared with the ESP32. The pad marked GND next
  to the A/B terminals on this board is **not** connected to power ground.
* **Exactly one 120 Ω terminator, at the far end of the line** (last fixture or a terminator
  plug). Remove the module's own 120 Ω (R0): a terminator at both ends acts like 60 Ω and halves
  the "1" level, which then fails (+0.17 V measured).
* Use the 5 V supply for the pull-up. Taken from 3.3 V, it gives less margin.

Measured on a Rigol DHO924 with this wiring (4.75 V from USB, 120 Ω at the far end only):

| | Measured | DMX512 needs |
|---|---|---|
| "1" bits / idle, A−B | +0.34 V (worst bit +0.27 V) | ≥ +0.2 V |
| "0" bits / break, A−B | −2.25 V | ≤ −0.2 V |
| Rising edge | driven to +2.2 V for ~470 ns, then held by the bias | |
| Overshoot / settling | ≤ 0.1 V, ~160 ns | |
| Bit time / break / MAB | 4.009 µs / 178 µs / 19–30 µs | 4 µs ±2 % / ≥ 88 µs / ≥ 8 µs |
| Frame rate | 40.0 Hz (25.00 ms ± 0.05 ms) | ≤ 44 Hz |

Results with the wrong wiring, for troubleshooting:

| Wiring | "1" level (A−B) |
|---|---|
| No bias resistors (module as shipped) | +0.03 to +0.07 V ❌ |
| Bias pull-down to the GND pad by A/B (not real GND) | +0.14 V ❌ |
| Bias OK, 120 Ω at both ends | +0.17 V ❌ |
| Bias OK, 120 Ω at far end only | **+0.34 V ✅** |

The margin shrinks a little with long cables and each extra fixture. If fixtures misbehave on
long runs, use a DE-controlled MAX485/MAX3485 board (`de_pin` setting), which drives "1" bits at
about +2 V and needs no bias resistors.

## Build & flash

Built with ESP-IDF v6.2 for a 16 MB flash ESP32-S3 (`~/esp/esp-idf`). Partitions (`partitions.csv`):
2 × 4 MB OTA app slots, `storage` (1 MB raw: channel names and scenes), `scripts` (3 MB SPIFFS:
effect scripts), `www` (1 MB SPIFFS: the web pages); the last ~3 MB are unused. The board's 2 MB
PSRAM holds the script engine's heap; it also runs without PSRAM, with a 64 KB script heap.

```bash
. ~/esp/esp-idf/export.sh
idf.py set-target esp32s3        # once
idf.py build
idf.py -p /dev/serial/by-id/usb-Espressif_USB_JTAG_serial_debug_unit_68:B6:B3:47:FF:1C-if00 flash monitor
```

`idf.py flash` writes the bootloader, partition table, app and the `www` image; it never touches
`nvs` (settings), `storage` (names, scenes) or `scripts`. A change to `partitions.csv` needs this USB
flash, because over-the-air updates can't change the partition table. Updating from a firmware
without the `scripts` partition (before 1.9.0) needs one USB flash: shrinking `storage` keeps the
names and scenes, and the first boot formats `scripts` (about 20 s) and writes the example scripts.
`idf.py flash` also resets the boot slot to `ota_0`; `idf.py app-flash` alone writes `ota_0` while
the bridge may be booting the other slot after an OTA update.

The build also needs **Node.js/npm**: CMake runs `npm ci` (first build, or when
`web/package-lock.json` changes) and `npm run build` in `web/`, which uses webpack to bundle each web
page with its CSS and JS into one minified, gzipped HTML file. The pages go into the `www` image
(`build/www.bin`), into the package `build/dmx_bridge_www.tar`, and into the firmware itself as a
fallback copy.
The pages are small [Preact](https://preactjs.com) apps written in JSX (compiled by esbuild-loader):
`web/src/index/` is the start page (script board), `web/src/settings/` the settings page, `web/src/console/` the fader console (`live.js` holds the
WebSocket connection and the 512-channel state), `web/src/scripts/` the script editor,
`web/src/common/` shared helpers. Edit them
there; `idf.py build` re-bundles them when they change. To build only the web UI:
`cd web && npm ci && npm run build` (output in `web/dist/`: `www/` and `dmx_bridge_www.tar`).

The script engine is [Duktape](https://duktape.org) 2.7.0, vendored in `components/duktape/` with a
few config changes (see its README).

Default pins, UART, AP password and hostname prefix are under `idf.py menuconfig` → *DMX Bridge*.
All of them except the AP password can also be changed in the web UI.

## First-time setup (Wi-Fi provisioning)

1. With no Wi-Fi stored, the bridge opens an access point **`DMX-Bridge-XXXX`**
   (password `dmxbridge`). XXXX is the end of its MAC address.
2. Join it with a phone or laptop. The captive portal opens the settings page; if it doesn't,
   browse to `http://192.168.4.1/settings`.
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

## Fader console (`/console`)

A lighting-desk style page at `http://dmx-bridge-XXXX.local/console` (button on the main page):

* Fader strips with value readout, an output meter (what really goes out, QLC+ included),
  **Flash** (momentary full) and **0**. As many faders per bank as fit the screen (4–5 on a phone
  in portrait, up to 16); page with ◀ ▶ or the arrow keys. Mouse wheel = fine adjust (Shift = ×10).
* **Master** fader scales all console faders. **DBO** is a momentary dead blackout of the console
  layer, and **Full** sets the master back to 100%. **Clear** (tap twice) zeroes all console faders.
* Console faders are merged **HTP** (highest takes precedence) with the Art-Net/sACN input, like a
  desk's manual layer. They never time out, so a channel you push up stays up until you pull it
  down, even when QLC+ stops.
* Several phones/tablets can be open at once. A WebSocket (`/ws`) keeps them in sync, at about
  15 updates/s.
* **Channel menu:** tap the name bar at the top of a strip to open its menu: set a name (up to
  24 bytes, UTF-8) or **Hide channel**. Enter saves, Esc cancels, Tab moves to the next strip.
  Once any channel has a name, **unnamed channels are hidden**, so the console shows just your
  fixtures' channels; naming a channel shows it. Named channels can be hidden from the menu too.
  Hidden channels are skipped when paging through banks; their DMX values are unchanged. A
  hidden-channels button in the top bar (crossed-out eye and count) shows them (dimmed) so you can name or **Show channel
  again**.
  Names and hidden flags are stored on the bridge, in the `storage` partition as two alternating
  copies so a power cut can't corrupt them. They're shared by every device and update live on all
  open consoles. API: `GET /api/names` (`{"names":{...},"hidden":[5,6]}`), `POST /api/names`
  with `{"1":"Front wash","2":""}` (empty = remove), `{"hidden":{"5":true,"6":false}}`,
  `{"show_all":true}` or `{"clear":true}` (names only).
  Serial console: `name 1 Front wash`, `names`, `hide 5`, `unhide 5` / `unhide all`.
* **Scenes:** up to 64 snapshots of all console faders, stored on the bridge (one flash sector each,
  CRC-checked). Tap a scene in the scene bar to recall it, using the **Fade s** time (0–60 s; the
  fade runs on the bridge, so every open console sees it). Moving a fader during a fade takes
  that channel out of the fade. **+ Save** stores the current faders in the next free slot. **Edit**
  shows all 64 slots to overwrite, rename or delete (tap twice). Keys 1–9 recall the first nine
  scenes. Scenes hold the console layer only; QLC+ input is not captured.
  API: `GET /api/scenes`, `POST /api/scenes` with
  `{"action":"recall|save|rename|delete","id":1-64,"name":"…","fade_ms":2000}`.
* Values can show as DMX (0–255) or %. The chosen bank and units are remembered per browser.

## Effect scripts (`/scripts`)

JavaScript effects that run on the bridge itself: coordinated pan/tilt movement, colour chases,
anything you can compute per frame. They run without QLC+ or a browser, and the running script
restarts after a reboot. The **Effect scripts** page has the editor (with line numbers, Ctrl+S,
error line jump), Run/Stop, the script's live parameter sliders and its log. The console's **FX**
button shows a bar to start and stop scripts and adjust their parameters.

```js
include('setup');                                     // heads = the rig, from setup.js
var phase = 0;
function frame(t, dt) {
  phase += param('speed', 0.15, 0, 1) * dt;           // live slider
  heads.forEach(function (h, i) {
    var a = 2 * Math.PI * (phase - i / 4);           // each head a quarter circle behind
    h.pan = 127.5 + 40 * Math.cos(a);
    h.tilt = 127.5 + 40 * Math.sin(a);
    h.dim = 255;
  });
}
```

Channels a script sets replace the network input; all others still follow QLC+. The lights are
described once, in a shared `scripts/setup.js` (fixture types, the rig and `ready()`), which
effects load with `include('setup')`. `scripts/cycle.js` plays a list of effects in turn, each for
a set time (and can play other cycles). Examples and the full API are in
[`scripts/`](scripts/README.md).

## Web UI

The start page (`/`) is a button board. **Effects** has one big button per effect script: tap one
to run it, and tap the running one to stop it. The running script's parameter sliders show below
the buttons. **Scenes** has one button per stored scene: tap one to recall it, with the fade time
set next to it (shared with the console's scene bar). **Status** below them shows Wi-Fi, the DMX
input, packet and frame rates and live bars for all 512 output channels.
From there, *Console* opens the fader console, *Scripts* the editor and *Settings* the settings
page (`/settings`):

* **Status:** Wi-Fi, IP, active source, packets/s, DMX frames/s and a live view of all 512
  output channels.
* **DMX input:** protocol, Art-Net universe (shown as Net/Sub/Uni), sACN universe, hold or
  blackout on signal loss, loss timeout, refresh rate (default 40 Hz; a full 512-slot frame
  allows about 44 Hz).
* **Hardware:** TX/DE/LED GPIOs, UART. Pin, Wi-Fi and hostname changes restart the bridge.

* **Firmware:** upload a new `build/dmx_bridge.bin` from the browser. It is written to the
  inactive OTA slot, verified, and booted. If the new firmware crashes before it finishes
  starting, the bootloader rolls back to the previous one on the next reset.
  The same card takes **`dmx_bridge_www.tar`** (from `build/` or `web/dist/`) to update only the
  web pages, without a restart; see *Web pages* below.

JSON API: `GET /api/status`, `GET/POST /api/config`, `GET /api/scan`, `POST /api/reboot`,
`POST /api/factory_reset`,
`POST /api/ota` (raw `.bin` as the body, e.g.
`curl --data-binary @build/dmx_bridge.bin http://dmx-bridge-XXXX.local/api/ota`),
`POST /api/www` (web package `.tar` as the body). Scripts: see [`scripts/README.md`](scripts/README.md).

### Web pages

The pages are built from `web/src/` (`index/` = script board, `settings/`, `console/` = fader
console, `scripts/` = script editor) into one gzipped HTML file each, served at `/`, `/settings`,
`/console` and `/scripts` with `Content-Encoding: gzip` (see
`main/assets.c`). They live on the `www` SPIFFS partition, and the firmware carries the copies it
was built with:

* **Web-only update:** upload `dmx_bridge_www.tar` (a plain tar of the `*.html.gz` pages and
  `manifest.json`). The bridge unpacks it to temporary files and only swaps
  them in once the whole archive arrived and checked out, so a broken upload leaves the old pages
  in place.
* **Newest wins:** each set has a build time in its `manifest.json`. The bridge serves whichever is
  newer — an uploaded package, or the pages inside the running firmware — so a firmware update
  brings its own newer UI, and a package older than the firmware's pages is refused.
* **Recovery:** `?builtin` on any page (`/settings?builtin`, `/console?builtin`…) always serves the firmware's own copy, e.g. if
  an uploaded UI is broken.
* The settings page's Firmware card shows which web UI is running. Browsers revalidate pages on
  every load (ETag from the package / firmware build), so an update shows up on the next reload.

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
| `name <ch> [text]` / `names` | set (no text = remove) / list channel names |
| `hide <ch>` / `unhide <ch\|all>` | hide / show channels on the web console |
| `scene list` / `save <n> [name]` / `recall <n> [fade s]` / `rename <n> <name>` / `delete <n>` | console scenes |
| `script [list]` / `run <name>` / `stop` / `log` / `param <name> <value>` | effect scripts |
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

## License

MIT — see [LICENSE](LICENSE).
