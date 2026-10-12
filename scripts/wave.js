// Wave: the heads nod one after another, like a wave rolling along the row, fanned out left to
// right. Lights and channels: setup.js.
include('setup');

var phase = 0;

function frame(t, dt) {
  var speed  = param('speed', 0.25, 0, 1);       // waves per second
  var height = param('height', 40, 0, 127, 1);   // nod, up and down from 'tilt'
  var lag    = param('lag', 0.2, 0, 1);          // delay from one head to the next, in waves
  var fan    = param('fan', 20, 0, 127, 1);      // pan spread across the row
  var pan    = param('pan', 128, 0, 255, 1);
  var tilt   = param('tilt', 127, 0, 255, 1);
  var dim    = param('dim', 255, 0, 255, 1);
  phase += speed * dt;

  ready();
  heads.forEach(function (h, i) {
    var x = heads.length > 1 ? i / (heads.length - 1) - 0.5 : 0;   // -0.5..0.5 along the row
    h.pan = clamp(pan + 2 * fan * x, 0, 255);
    h.tilt = clamp(tilt + height * Math.sin(2 * Math.PI * (phase - i * lag)), 0, 255);
    h.dim = dim;
  });
}
