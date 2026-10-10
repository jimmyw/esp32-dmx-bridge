// Fan circle: the heads draw circles, each a bit behind the previous one, so the beams chase
// each other around. Lights and channels: setup.js.
include('setup');

var phase = 0;   // in circles; integrated so changing speed doesn't make the heads jump

function frame(t, dt) {
  var speed  = param('speed', 0.15, 0, 1);       // circles per second
  var size   = param('size', 40, 0, 127, 1);     // radius, in DMX steps
  var spread = param('spread', 0.25, 0, 1);      // offset between heads, in circles
  var pan    = param('pan', 127, 0, 255, 1);     // centre
  var tilt   = param('tilt', 127, 0, 255, 1);
  var dim    = param('dim', 255, 0, 255, 1);
  phase += speed * dt;

  ready();
  heads.forEach(function (h, i) {
    var a = 2 * Math.PI * (phase - i * spread);
    h.pan = clamp(pan + size * Math.cos(a), 0, 255);
    h.tilt = clamp(tilt + size * Math.sin(a), 0, 255);
    h.dim = dim;
  });
}
