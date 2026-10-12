// Colour chase: every head steps through the colour wheel, one colour apart, while the LED
// ring follows and the heads sway slowly up and down. Pan is held at 'pan': left unset it would
// sit at 0, the end of the travel, where a head can push against its stop and lose its place.
// Lights, channels and colour positions: setup.js.
include('setup');

var pos = 0;
var phase = 0;

function frame(t, dt) {
  var rate  = param('rate', 1, 0.1, 8);          // colour steps per second
  var dim   = param('dim', 255, 0, 255, 1);
  var gobo  = param('gobo', 0, 0, 71, 1);        // fixed gobo (0-71)
  var speed = param('speed', 0.15, 0, 1);        // sways per second
  var size  = param('size', 40, 0, 127, 1);      // sway, up and down from 'tilt'
  var tilt  = param('tilt', 127, 0, 255, 1);
  var pan   = param('pan', 128, 0, 255, 1);      // where they point, held still
  pos += rate * dt;
  phase += speed * dt;
  var step = Math.floor(pos);
  var a = 2 * Math.PI * phase;

  heads.forEach(function (h, i) {
    h.color = WHEEL[(step + i) % WHEEL.length];
    h.strip = RING[(step + i + 3) % RING.length];
    h.tilt = clamp(tilt + size * Math.sin(a), 0, 255);
    h.pan = pan;
    h.gobo = gobo;
    h.dim = dim;
    h.strobe = 0;
    h.mode = 0;
  });
}
