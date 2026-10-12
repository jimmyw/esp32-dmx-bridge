// Spectrum: the row is a live spectrum. The first head follows the bass, the last the highs:
// each one lifts and brightens with its part of the sound. Needs the microphone.
// Lights and channels: setup.js.
include('setup');

var lvl = heads.map(function () { return 0; });

function frame(t, dt) {
  var lift   = param('lift', 50, 0, 127, 1);     // how far a head rises at full level
  var tilt   = param('tilt', 150, 0, 255, 1);    // where it rests
  var fan    = param('fan', 25, 0, 127, 1);
  var pan    = param('pan', 128, 0, 255, 1);
  var base   = param('base', 20, 0, 255, 1);     // dimmer when quiet
  var smooth = param('smooth', 0.1, 0, 1);       // seconds of smoothing: higher is calmer
  var n = heads.length, per = audio.bands.length / n;
  var k = smooth > 0 ? 1 - Math.exp(-dt / smooth) : 1;

  ready();
  heads.forEach(function (h, i) {
    var sum = 0;
    for (var b = Math.floor(i * per); b < Math.floor((i + 1) * per); b++) sum += audio.bands[b];
    lvl[i] += (sum / Math.max(1, Math.floor((i + 1) * per) - Math.floor(i * per)) - lvl[i]) * k;
    var x = n > 1 ? i / (n - 1) - 0.5 : 0;
    h.pan = clamp(pan + 2 * fan * x, 0, 255);
    h.tilt = clamp(tilt - lift * lvl[i], 0, 255);
    h.color = WHEEL[(i * 2) % WHEEL.length];
    h.strip = RING[Math.floor(i * RING.length / n)];
    h.dim = base + (255 - base) * lvl[i];
  });
}
