// Helpers available to every effect script (evaluated before the script). Duktape: ES5.1.

function clamp(v, lo, hi) { return v < lo ? lo : v > hi ? hi : v; }
function lerp(a, b, f) { return a + (b - a) * f; }
function frac(x) { return x - Math.floor(x); }

// Waves over a phase in cycles (1.0 = one period), all 0..1 and starting at 0.
function sine(x) { return 0.5 - 0.5 * Math.cos(2 * Math.PI * x); }
function tri(x) { x = frac(x); return x < 0.5 ? 2 * x : 2 - 2 * x; }
function saw(x) { return frac(x); }
function square(x) { return frac(x) < 0.5 ? 1 : 0; }

// h 0..1 (wraps), s and v 0..1 -> [r, g, b] 0..255
function hsv(h, s, v) {
  h = frac(h) * 6;
  var i = Math.floor(h), f = h - i;
  var p = v * (1 - s), q = v * (1 - s * f), t = v * (1 - s * (1 - f));
  var rgb = [[v, t, p], [q, v, p], [p, v, t], [p, q, v], [t, p, v], [v, p, q]][i % 6];
  return [rgb[0] * 255, rgb[1] * 255, rgb[2] * 255];
}

/*
 * fixture(address, layout): an object whose properties write DMX channels.
 *   layout maps a property to its channel within the fixture (1 = first channel), or to
 *   [coarse, fine] for 16-bit channels, e.g. { pan: [1, 2], tilt: [3, 4], dim: 8 }.
 *   Values are DMX levels 0..255; on 16-bit channels the fraction goes to the fine channel.
 */
function fixture(address, layout) {
  var f = { address: address };
  Object.keys(layout).forEach(function (key) {
    var off = layout[key];
    if (off instanceof Array) {
      var coarse = address + off[0] - 1, fine = address + off[1] - 1;
      Object.defineProperty(f, key, {
        get: function () { return (get(coarse) * 256 + get(fine)) / 257; },
        set: function (v) { setFine(coarse, v, fine); },
        enumerable: true
      });
    } else {
      var ch = address + off - 1;
      Object.defineProperty(f, key, {
        get: function () { return get(ch); },
        set: function (v) { set(ch, v); },
        enumerable: true
      });
    }
  });
  return f;
}

/*
 * effect(name): load another effect script as a child, with its own variables and its own live
 * parameters (shown as "<name>.<param>"). Returns { name, frame(t, dt) }: the caller runs it by
 * calling frame(), e.g. a cycle that plays effects in turn. Children may load children too.
 */
var __loading = {};
function effect(name) {
  if (__loading[name]) throw new Error('effect: ' + name + ' loads itself');
  __loading[name] = true;
  var e;
  try {
    e = __load(name)(function (n, d, min, max, step) {
      return param(name + '.' + n, d, min, max, step);
    });
  } finally {
    delete __loading[name];
  }
  if (typeof e.frame !== 'function') throw new TypeError('effect: ' + name + ' has no frame(t, dt) function');
  e.name = name;
  return e;
}
