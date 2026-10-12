// Beat flash: on every beat one head flashes full and fades out, the next head on the next
// beat; 'random' picks the head at random instead. The colour steps once round the row.
// Plays to the music (or the timed Fallback beat). Lights and channels: setup.js.
include('setup');

var level = heads.map(function () { return 0; });
var next = 0, step = 0;

function frame(t, dt) {
  var decay  = param('decay', 0.3, 0.05, 2);     // seconds to fade
  var random = param('random', 0, 0, 1, 1);      // 1: a random head each beat
  var base   = param('base', 0, 0, 255, 1);      // dimmer between flashes
  var fan    = param('fan', 25, 0, 127, 1);
  var pan    = param('pan', 128, 0, 255, 1);
  var tilt   = param('tilt', 110, 0, 255, 1);
  if (audio.beat) {
    var k = random ? Math.floor(Math.random() * heads.length) : next;
    level[k] = 1;
    next = (k + 1) % heads.length;
    if (next === 0) step++;
  }
  var fade = Math.exp(-dt * 4.6 / decay);        // to 1% after 'decay' seconds

  ready();
  heads.forEach(function (h, i) {
    var x = heads.length > 1 ? i / (heads.length - 1) - 0.5 : 0;
    h.pan = clamp(pan + 2 * fan * x, 0, 255);
    h.tilt = tilt;
    h.color = WHEEL[step % WHEEL.length];
    h.strip = RING[(step + i) % RING.length];
    h.dim = base + (255 - base) * level[i];
    level[i] *= fade;
  });
}
