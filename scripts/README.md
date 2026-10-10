# Effect scripts

Small JavaScript programs that run **on the bridge** and generate DMX by themselves: coordinated
movement, colour chases, anything you can compute. They keep running without QLC+ or a browser,
and the running one restarts after a power cut.

Write and run them on the bridge's **Effect scripts** page (`http://<bridge>/scripts`), or start
them from the **FX** bar in the fader console. The files in this directory are the examples that
come with the firmware; a fresh bridge gets a copy of each.

## How a script runs

```js
var head = fixture(1, { pan: [1, 2], tilt: [3, 4], dim: 8 });   // runs once, on start

function frame(t, dt) {                                          // runs every DMX frame (40/s)
  var speed = param('speed', 0.2, 0, 1);                         // a live slider
  head.dim = 255;
  head.pan = 127.5 + 60 * Math.sin(2 * Math.PI * speed * t);
}
```

* Top-level code runs once when the script starts. Then `frame(t, dt)` runs at the DMX refresh rate:
  `t` is seconds since the start, `dt` seconds since the previous frame.
* One script runs at a time. Starting another stops the current one. Saving the running script
  (or one that just failed) restarts it with the new code.
* Channels the script sets **replace** the network input (Art-Net / sACN from QLC+) on those
  channels. Every other channel still follows QLC+. The web console's faders still merge on top,
  highest value wins, so leave them at 0 for channels a script drives.
* Errors stop the script. The message, with its line number, shows on the scripts page, in the
  console's FX bar and in the log.
* Limits: top-level code may run 3 s and each `frame()` call 1 s; past that the script is stopped
  as an endless loop. The script gets a 768 KB heap (PSRAM), and scripts can be up to 32 KB each.
* Duktape implements **ES5.1**: use `var` and `function`. Arrow functions, `let`/`class` and
  template strings don't parse.

## API

| | |
|---|---|
| `set(ch, v)` | Set channel 1–512 to `v` (0–255, rounded and clamped). The script owns the channel from then on. |
| `setFine(ch, v, [fineCh])` | 16-bit channel: `v` is 0–255 with a fraction; the fine part goes to `fineCh` (default `ch + 1`). |
| `get(ch)` | The value this script last set on `ch`. |
| `input(ch)` | The network input on `ch`. For example, let a QLC+ fader on a spare channel drive the speed. |
| `fader(ch)` | The web console fader of `ch` (0–255). |
| `release(ch)` / `release()` | Hand one channel, or all of them, back to the network input. |
| `param(name, def, [min], [max], [step])` | Declare a live parameter and return its current value. The first call creates a slider on the scripts page and in the console FX bar; call it inside `frame()` to follow changes. Values are saved per script (`<name>.json`) and come back on the next start. Defaults: `min` 0, `max` `max(1, 2 × def)`, `step` continuous. At most 16 per script; names up to 15 characters. |
| `print(...)` | Write a line to the log on the scripts page (and the serial console). |

From the prelude (`main/script_prelude.js`):

| | |
|---|---|
| `fixture(address, layout)` | Object whose properties write channels. `layout` maps a name to the channel within the fixture (1 = first), or to `[coarse, fine]` for 16-bit. `fixture(13, { pan: [1, 2], dim: 8 })`: `h.pan = 127.5` writes ch 13 + 14, and `h.dim = 255` writes ch 20. |
| `sine(x)`, `tri(x)`, `saw(x)`, `square(x)` | Waves from 0 to 1 over a phase `x` in cycles (1 = one period). They all start at 0. |
| `clamp(v, lo, hi)`, `lerp(a, b, f)`, `frac(x)` | |
| `hsv(h, s, v)` | `[r, g, b]` 0–255 for hue 0–1 (wraps), saturation and value 0–1 |

All of `Math`, `JSON`, `Date` and so on are there too.

## Tips

* **Moving heads:** put the fixture's pan/tilt speed channel at 0 (fastest) and stream the
  positions every frame. Use the 16-bit pan/tilt mode where the fixture has one; 8-bit circles look
  steppy.
* **Speed changes without jumps:** integrate the phase, `phase += speed * dt`, instead of computing
  `speed * t`. Otherwise every change of `speed` makes the heads jump.
* **Fixture layout:** the examples use the mini moving heads' 12-channel mode (pan, pan fine,
  tilt, tilt fine, colour, gobo, strobe, dimmer, speed, auto mode, -, LED ring) at addresses 1, 13,
  25 and 37. Change `LAYOUT` and the address list at the top for other fixtures.

## Files and API

Scripts live on the bridge's `scripts` partition (3 MB SPIFFS) as `<name>.js`, with names of
1–24 letters, digits, `-` or `_`.

```bash
curl http://<bridge>/scripts/fan-circle.js                     # download
curl -T my-effect.js http://<bridge>/scripts/my-effect.js      # upload / replace
curl -d '{"action":"run","name":"my-effect"}' http://<bridge>/api/scripts
curl -d '{"params":{"speed":0.3}}' http://<bridge>/api/scripts
curl -d '{"action":"stop"}' http://<bridge>/api/scripts
curl http://<bridge>/api/scripts                               # status, params, file list
curl 'http://<bridge>/api/scripts/log?since=0'
```

`{"action":"delete","name":…}` removes a script. On the serial console: `script [list]`,
`script run <name>`, `script stop`, `script log`, `script param <name> <value>`.

If the bridge crashes or a watchdog fires while a script is starting at boot, the next boot leaves
it stopped instead of looping. Run it again to retry.
