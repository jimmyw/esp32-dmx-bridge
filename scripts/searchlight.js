// Searchlight: open white beams sweep slowly across the room in a wide arc, the heads close
// behind each other like searchlights over a premiere. Lights and channels: setup.js.
include('setup');

var phase = 0;

function frame(t, dt) {
  var speed  = param('speed', 0.06, 0, 0.5);     // sweeps there and back per second
  var width  = param('width', 80, 0, 127, 1);
  var spread = param('spread', 0.04, 0, 0.5);    // delay from one head to the next, in sweeps
  var pan    = param('pan', 128, 0, 255, 1);
  var tilt   = param('tilt', 90, 0, 255, 1);
  var bob    = param('bob', 10, 0, 64, 1);       // a slight rise and fall along the sweep
  var dim    = param('dim', 255, 0, 255, 1);
  phase += speed * dt;

  ready();
  heads.forEach(function (h, i) {
    var a = 2 * Math.PI * (phase - i * spread);
    h.pan = clamp(pan + width * Math.sin(a), 0, 255);
    h.tilt = clamp(tilt + bob * Math.cos(2 * a), 0, 255);
    h.color = WHEEL[0];
    h.gobo = 0;
    h.dim = dim;
  });
}
