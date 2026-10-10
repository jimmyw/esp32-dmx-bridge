// Script API reference (keep in step with main/script.c, main/script_prelude.js and
// scripts/README.md).
const API = [
  ['frame(t, dt)', 'You define this: called every DMX frame. t = seconds since the script started, dt = seconds since the last frame.'],
  ['set(ch, v)', 'Set channel 1-512 to 0-255. The script owns the channel from then on: it replaces the network (QLC+) input there.'],
  ['setFine(ch, v, [fineCh])', '16-bit channel: v 0-255 with a fraction; the fraction goes to fineCh (default ch + 1).'],
  ['get(ch)', 'The value this script last set.'],
  ['input(ch)', 'The network input (Art-Net / sACN) on that channel – e.g. let QLC+ drive a spare channel.'],
  ['fader(ch)', 'The web console fader of that channel (0-255).'],
  ['release(ch) / release()', 'Hand one channel / all channels back to the network input.'],
  ['param(name, def, [min], [max], [step])', 'Declare a live slider (shown here and in the console FX bar) and return its value. Call it in frame() to follow changes. Values are saved per script.'],
  ['print(...)', 'Write a line to the log below.'],
  ['fixture(address, layout)', 'Object whose properties write channels: fixture(13, { pan: [1, 2], dim: 8 }) – head.pan = 127.5 sets 13+14 (16-bit), head.dim = 255 sets 20.'],
  ['sine(x) tri(x) saw(x) square(x)', 'Waves 0..1 over a phase in cycles (1 = one period).'],
  ['clamp(v, lo, hi) lerp(a, b, f) frac(x) hsv(h, s, v)', 'Helpers; hsv() returns [r, g, b] 0-255.'],
];

export function Help() {
  return (
    <section class="card">
      <details>
        <summary><h2 style={{ display: 'inline' }}>Script API</h2></summary>
        <p class="hint">JavaScript (ES5.1: <code>var</code> and <code>function</code>; no arrow functions,
          classes or template strings), run by Duktape on the bridge. Top-level code runs once on start
          (max 3 s); each <code>frame()</code> call should be quick – one that runs over 1 s stops the script. Channels the script sets replace
          the network input; console faders still add on top (highest wins). A running script restarts
          after a reboot, and when you save it (so does one that just failed).</p>
        <table class="api">
          <tbody>
            {API.map(([sig, doc]) => <tr key={sig}><td><code>{sig}</code></td><td>{doc}</td></tr>)}
          </tbody>
        </table>
      </details>
    </section>
  );
}
