# Effect scripts

Small JavaScript programs that run **on the bridge** and generate DMX by themselves: coordinated
movement, colour chases, anything you can compute. They keep running without QLC+ or a browser,
and the running one restarts after a power cut.

Write and run them on the bridge's **Effect scripts** page (`http://<bridge>/scripts`), or start
them from the **FX** bar in the fader console. The files in this directory are the examples that
come with the firmware; a fresh bridge gets a copy of each.

## How a script runs

```js
include('setup');                            // the lights: heads, TYPES, ready() from setup.js
var phase = 0;                               // top-level code runs once, on start

function frame(t, dt) {                      // runs every DMX frame (40/s)
  phase += param('speed', 0.2, 0, 1) * dt;   // a live slider
  ready();
  heads.forEach(function (h, i) {
    h.dim = 255;
    h.pan = 127.5 + 60 * Math.sin(2 * Math.PI * (phase + i / heads.length));
  });
}
```

## The light setup: `setup.js`

The lights are described once, in `setup.js`, and every effect loads it with
`include('setup')`:

* `TYPES`: one channel layout per kind of light, mapping property names to channels within the
  fixture. It has the mini moving head in 12- and 10-channel mode and the 13-channel head.
* `heads`: the rig, one `fixture(address, TYPES.…)` per light.
* `ready()`: puts every head under DMX control (no built-in program, no strobe, fastest
  pan/tilt).
* `WHEEL` / `RING`: colour positions for the colour effects.

Change a channel, readdress a light or add a new type there, and every effect follows. Saving
`setup.js` restarts the running effect with it. Use the same property names across types (`pan`,
`tilt`, `dim`, `color`, …) and an effect drives any mix of lights. Setting a property a type
doesn't have does nothing.

Files named `setup` or starting with `_` (say `_colors.js`) are **shared files**: the editor lists
them under *Shared*, they don't appear as effects on the start page or in the FX bar, and they
can't be run on their own. An error inside one is reported as `setup.js line 7: …`, and clicking
it opens that file at that line.

A bridge that already had scripts before this change gets `setup.js` once, on the first boot of the
new firmware; your existing files are not touched.

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
  as an endless loop. Up to 32 sliders per script (children's included). The script gets a 768 KB heap (PSRAM), and scripts can be up to 32 KB each.
* Duktape implements **ES5.1**: use `var` and `function`. Arrow functions, `let`/`class` and
  template strings don't parse.

## Playing effects in turn: `cycle.js`

`cycle.js` plays other effects one after another, each for `seconds` (a slider, default 20), then
the next:

```js
var effects = [effect('fan-circle'), effect('figure-eight'), effect('color-chase')];
```

Each effect in the list runs as a **child**, loaded with `effect(name)`:

* **Own scope:** it keeps its own variables, so its motion picks up where it left off on its next
  turn.
* **Own sliders:** it keeps its sliders, named `<effect>.<param>` (`fan-circle.speed`). They are
  saved with the cycle and grouped per effect on the start page and the scripts page; the
  console's FX bar shows just the cycle's own.
* **Nesting:** a child can be a cycle itself, e.g. `[effect('cycle'), effect('color-chase')]` plays the whole
  cycle, then the colour chase.
* **Changes:** saving a child restarts the cycle with it, and an error in a child reads
  `fan-circle.js line 12: …`.

Channels an effect set keep their last value when the next one takes over, so a colour chase after
a circle holds the heads where the circle left them.

## Music: the `audio` object

With an INMP441 microphone on the bridge (see the main README), every frame gets the analysis of
what it hears. When it hears nothing, or there is no microphone, the demo track plays into the
same analysis at the *Demo* tempo on the start page's Sound card; the **Demo** button forces it.

| | |
|---|---|
| `audio.on` | a microphone is configured, or the demo track plays |
| `audio.signal` | sound above the noise gate (Settings) |
| `audio.level`, `audio.bass`, `audio.mid`, `audio.high` | loudness overall and in 40–150 Hz / 150–2000 Hz / 2–8 kHz, 0..1 |
| `audio.bands[0..15]` | 16 bands, 40 Hz to 10 kHz, 0..1 |
| `audio.beat` | `true` on the frame of each beat: a steady grid locked to the music once there's a tempo, before that each detected kick |
| `audio.beats` | beats counted so far |
| `audio.bpm` | the beat's tempo: the music's (or the demo's), or the *Demo* tempo while there's sound without a beat; 0 if nothing |
| `audio.locked` | `true` while the beat follows the sound, `false` when it runs on the *Demo* tempo |
| `audio.phase` | 0..1 through the current beat |
| `audio.time` | the beat clock: beats elapsed since the script started, smooth (6.5 = halfway through the seventh). With nothing to follow it runs on at 120 BPM. |

All levels are auto-gained, so they reach 1 whatever the room volume; below the noise gate they
fall to 0. The beat values follow the *Offset* slider on the start page's Sound card, which moves
beats earlier or later to line the lights up with the music. Typical uses:

```js
if (audio.beat) step++;                         // something new on every beat
h.dim = 255 * (0.3 + 0.7 * audio.bass);         // pump with the kick
var p = audio.time / 4;                         // one motion cycle every 4 beats, in time
```

`beat-pulse.js` does all three. `cycle.js` has a `beats` slider to switch effects every N beats
instead of every N seconds.

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
| `effect(name)` | Load another effect as a child: its own variables, and sliders named `<name>.<param>`. Returns `{ name, frame(t, dt) }`; call `frame()` to run it. A script can't load itself. |
| `include(name)` | Run `<name>.js` first, in the same global scope, once per start; repeated and circular includes do nothing. Up to 8 files. |

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
* **Fixture layout:** the examples drive `heads` from `setup.js`: four mini moving heads in
  12-channel mode at addresses 1, 13, 25 and 37. Change the rig there.

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
