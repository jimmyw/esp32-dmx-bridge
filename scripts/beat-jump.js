// Beat jump: every few beats all heads jump to new random positions and a new colour, with the
// dimmer pumping on the bass. Plays to the music (or the timed Fallback beat).
// Lights and channels: setup.js.
include('setup');

var target = heads.map(function () { return [128, 120]; });
var count = 0, step = 0;

function frame(t, dt) {
  var every  = param('every', 2, 1, 16, 1);      // beats per jump
  var width  = param('width', 50, 0, 127, 1);    // how far a jump can go
  var height = param('height', 30, 0, 127, 1);
  var pan    = param('pan', 128, 0, 255, 1);
  var tilt   = param('tilt', 115, 0, 255, 1);
  var depth  = param('depth', 0.5, 0, 1);        // how far the dimmer follows the bass
  if (audio.beat && ++count >= every) {
    count = 0;
    step++;
    target.forEach(function (p) {
      p[0] = pan + width * (2 * Math.random() - 1);
      p[1] = tilt + height * (2 * Math.random() - 1);
    });
  }

  ready();
  heads.forEach(function (h, i) {
    h.pan = clamp(target[i][0], 0, 255);
    h.tilt = clamp(target[i][1], 0, 255);
    h.color = WHEEL[(step + i) % WHEEL.length];
    h.strip = RING[(step * 3 + i) % RING.length];
    h.dim = 255 * (1 - depth + depth * audio.bass);
  });
}
