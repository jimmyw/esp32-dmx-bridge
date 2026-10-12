// Fan and converge: the beams open into a wide fan, then close onto one spot, and open again,
// in time with the music (or the timed Fallback beat). The colour changes each time they meet.
// Lights and channels: setup.js.
include('setup');

var step = 0, met = false;

function frame(t, dt) {
  var beats = param('beats', 8, 1, 32, 1);       // beats for open, close and open again
  var fan   = param('fan', 60, 0, 127, 1);       // how wide the fan opens
  var pan   = param('pan', 128, 0, 255, 1);      // the spot they meet on
  var tilt  = param('tilt', 120, 0, 255, 1);
  var lift  = param('lift', 20, 0, 127, 1);      // the open fan also rises by this much
  var dim   = param('dim', 255, 0, 255, 1);
  var open = 1 - sine(audio.time / beats + 0.5); // 1 = open fan, 0 = all on the spot
  if (open < 0.02 && !met) step++;
  met = open < 0.02;

  ready();
  heads.forEach(function (h, i) {
    var x = heads.length > 1 ? i / (heads.length - 1) - 0.5 : 0;
    h.pan = clamp(pan + 2 * fan * x * open, 0, 255);
    h.tilt = clamp(tilt - lift * open, 0, 255);
    h.color = WHEEL[step % WHEEL.length];
    h.strip = RING[(step + i) % RING.length];
    h.dim = dim;
  });
}
