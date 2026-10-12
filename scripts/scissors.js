// Scissors: the heads sweep side to side, every second one the other way, so the beams cross
// in the middle. The colour steps at every crossing. Lights and channels: setup.js.
include('setup');

var phase = 0, last = 0, step = 0;

function frame(t, dt) {
  var speed = param('speed', 0.2, 0, 1);         // sweeps there and back per second
  var width = param('width', 50, 0, 127, 1);
  var pan   = param('pan', 128, 0, 255, 1);      // where the beams cross
  var tilt  = param('tilt', 110, 0, 255, 1);
  var dim   = param('dim', 255, 0, 255, 1);
  phase += speed * dt;
  var s = Math.sin(2 * Math.PI * phase);
  if ((s >= 0) !== (last >= 0)) step++;          // crossing the middle
  last = s;

  ready();
  heads.forEach(function (h, i) {
    var dir = i % 2 ? -1 : 1;
    h.pan = clamp(pan + dir * width * s, 0, 255);
    h.tilt = tilt;
    h.color = WHEEL[step % WHEEL.length];
    h.strip = RING[(step + i) % RING.length];
    h.dim = dim;
  });
}
