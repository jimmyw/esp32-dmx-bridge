// Ballyhoo: every head wanders on its own smooth, never-repeating path, and the colours drift
// from head to head every few seconds. Lights and channels: setup.js.
include('setup');

var phase = 0, pos = 0;
// Two slightly detuned sines per axis and head give the wandering paths.
var F = heads.map(function (h, i) {
  return [1 + 0.31 * i, 1.7 + 0.23 * i, 1.13 + 0.17 * i, 1.9 - 0.13 * i];
});

function frame(t, dt) {
  var speed = param('speed', 0.15, 0, 1);
  var width = param('width', 50, 0, 127, 1);     // pan reach either side of 'pan'
  var height = param('height', 30, 0, 127, 1);   // tilt reach
  var pan   = param('pan', 128, 0, 255, 1);
  var tilt  = param('tilt', 120, 0, 255, 1);
  var every = param('every', 4, 0.5, 20);        // seconds per colour change
  var dim   = param('dim', 255, 0, 255, 1);
  phase += speed * dt;
  pos += dt / every;
  var step = Math.floor(pos);
  var a = 2 * Math.PI * phase;

  ready();
  heads.forEach(function (h, i) {
    var f = F[i];
    h.pan = clamp(pan + width * (0.6 * Math.sin(f[0] * a + i) + 0.4 * Math.sin(f[1] * a + 2 * i)), 0, 255);
    h.tilt = clamp(tilt + height * (0.6 * Math.sin(f[2] * a + 3 * i) + 0.4 * Math.sin(f[3] * a)), 0, 255);
    h.color = WHEEL[(step + 2 * i) % WHEEL.length];
    h.strip = RING[(step + i) % RING.length];
    h.dim = dim;
  });
}
