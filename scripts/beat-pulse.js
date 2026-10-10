// Beat pulse: plays to the music (microphone, or the Demo track on the start page). The colour
// steps on every beat, the dimmer pumps with the bass, and the heads sweep side to side locked
// to the beat. Lights and channels: setup.js.
include('setup');

var step = 0;

function frame(t, dt) {
  var beatsPerSweep = param('sweep', 4, 1, 16, 1);   // beats for one sweep there and back
  var width = param('width', 50, 0, 127, 1);
  var depth = param('depth', 0.7, 0, 1);              // how far the dimmer follows the bass
  if (audio.beat) step++;

  var p = audio.time / beatsPerSweep;                 // sweeps elapsed, locked to the beat
  ready();
  heads.forEach(function (h, i) {
    h.pan = 127.5 + width * Math.sin(2 * Math.PI * (p + i / (2 * heads.length)));
    h.tilt = 110 + 25 * tri(p * 2);
    h.color = WHEEL[(step + i) % WHEEL.length];
    h.strip = RING[(step + i) % RING.length];
    h.dim = 255 * (1 - depth + depth * audio.bass);
  });
}
