// Colour chase: every head steps through the colour wheel, one colour apart, while the LED
// ring follows. Heads hold still where the pan/tilt faders put them (console or QLC+).
// Mini moving heads in 12-channel mode at addresses 1, 13, 25 and 37.

var LAYOUT = { color: 5, gobo: 6, strobe: 7, dim: 8, mode: 10, strip: 12 };
var heads = [1, 13, 25, 37].map(function (a) { return fixture(a, LAYOUT); });

// Colour wheel (ch 5: 0-139) and LED ring (ch 12: 5-109, seven colours). Tune these to the
// positions of your fixture.
var WHEEL = [0, 20, 40, 60, 80, 100, 120];
var RING  = [10, 25, 40, 55, 70, 85, 100];

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
