// Colour chase: every head steps through the colour wheel, one colour apart, while the LED
// ring follows. Heads hold still where the pan/tilt faders put them (console or QLC+).
// Lights, channels and colour positions: setup.js.
include('setup');

var pos = 0;

function frame(t, dt) {
  var rate = param('rate', 1, 0.1, 8);          // colour steps per second
  var dim  = param('dim', 255, 0, 255, 1);
  var gobo = param('gobo', 0, 0, 71, 1);        // fixed gobo (0-71)
  pos += rate * dt;
  var step = Math.floor(pos);

  heads.forEach(function (h, i) {
    h.color = WHEEL[(step + i) % WHEEL.length];
    h.strip = RING[(step + i + 3) % RING.length];
    h.gobo = gobo;
    h.dim = dim;
    h.strobe = 0;
    h.mode = 0;
  });
}
