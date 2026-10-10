// Figure eight: the heads trace an 8 (pan at one speed, tilt at double), every second head
// mirrored, with the dimmer breathing along.
// Mini moving heads in 12-channel mode at addresses 1, 13, 25 and 37.

var LAYOUT = { pan: [1, 2], tilt: [3, 4], color: 5, gobo: 6, strobe: 7, dim: 8,
               speed: 9, mode: 10 };
var heads = [1, 13, 25, 37].map(function (a) { return fixture(a, LAYOUT); });

var phase = 0;

function frame(t, dt) {
  var speed   = param('speed', 0.1, 0, 1);       // eights per second
  var width   = param('width', 60, 0, 127, 1);
  var height  = param('height', 30, 0, 127, 1);
  var mirror  = param('mirror', 1, 0, 1, 1);     // 1: odd heads run mirrored
  var breathe = param('breathe', 0.5, 0, 1);     // how deep the dimmer dips
  phase += speed * dt;

  heads.forEach(function (h, i) {
    var a = 2 * Math.PI * phase;
    var dir = (mirror && i % 2) ? -1 : 1;
    h.pan = 127.5 + dir * width * Math.sin(a);
    h.tilt = 127.5 + height * Math.sin(2 * a);
    h.dim = 255 * (1 - breathe * sine(phase * 2 + i / heads.length));
    h.speed = 0;
    h.mode = 0;
    h.strobe = 0;
  });
}
