// Rainbow fan: a calm look. The heads stand in a fan and breathe gently up and down while the
// colours roll slowly along the row, on the colour wheel and on the LED ring.
// Lights and channels: setup.js.
include('setup');

var pos = 0, phase = 0;

function frame(t, dt) {
  var rate    = param('rate', 0.3, 0, 4);        // colour steps per second
  var fan     = param('fan', 30, 0, 127, 1);
  var pan     = param('pan', 128, 0, 255, 1);
  var tilt    = param('tilt', 120, 0, 255, 1);
  var breathe = param('breathe', 8, 0, 64, 1);   // gentle nod
  var gobo    = param('gobo', 0, 0, 71, 1);
  var dim     = param('dim', 255, 0, 255, 1);
  pos += rate * dt;
  phase += 0.08 * dt;
  var step = Math.floor(pos);

  ready();
  heads.forEach(function (h, i) {
    var x = heads.length > 1 ? i / (heads.length - 1) - 0.5 : 0;
    h.pan = clamp(pan + 2 * fan * x, 0, 255);
    h.tilt = clamp(tilt + breathe * Math.sin(2 * Math.PI * (phase + i / heads.length)), 0, 255);
    h.color = WHEEL[(step + i) % WHEEL.length];
    h.strip = RING[(step + i + 1) % RING.length];
    h.gobo = gobo;
    h.dim = dim;
  });
}
