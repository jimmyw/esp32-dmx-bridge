// Knight rider: a light runs back and forth along the row with a soft tail, while the heads
// stand still in a fan. 'sync' 1 times each run to the beat. Lights and channels: setup.js.
include('setup');

var phase = 0;

function frame(t, dt) {
  var speed = param('speed', 0.5, 0, 2);         // runs there and back per second
  var sync  = param('sync', 0, 0, 1, 1);         // 1: one run there and back every 'beats'
  var beats = param('beats', 4, 1, 16, 1);
  var tail  = param('tail', 0.8, 0.1, 3);        // tail length, in heads
  var color = param('color', 3, 0, WHEEL.length - 1, 1);
  var fan   = param('fan', 30, 0, 127, 1);
  var pan   = param('pan', 128, 0, 255, 1);
  var tilt  = param('tilt', 120, 0, 255, 1);
  phase += speed * dt;
  var p = sync ? audio.time / beats : phase;
  var n = heads.length;
  var at = tri(p) * (n - 1);                     // where the light is, 0..n-1 along the row

  ready();
  heads.forEach(function (h, i) {
    var x = n > 1 ? i / (n - 1) - 0.5 : 0;
    h.pan = clamp(pan + 2 * fan * x, 0, 255);
    h.tilt = tilt;
    h.color = WHEEL[Math.min(color, WHEEL.length - 1)];
    h.strip = RING[color % RING.length];
    h.dim = 255 * Math.exp(-Math.pow((i - at) / tail, 2));
  });
}
