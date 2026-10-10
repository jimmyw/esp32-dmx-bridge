// Shared light setup: fixture types and the rig. Effects load it with include('setup'), so a
// channel change or a new light is made here, once, for every effect. Saving this file restarts
// the running effect with it.

// Fixture types: property -> channel within the fixture (1 = its first channel), or
// [coarse, fine] for 16-bit channels. Writing a property sends a DMX level 0-255 (on 16-bit
// channels the fraction goes to the fine channel). Use the same property names across types
// (pan, tilt, dim, color, ...) and effects work on any of them; setting a property a type
// doesn't have does nothing.
var TYPES = {
  // Mini moving head gobo light, 12-channel mode
  miniHead12: { pan: [1, 2], tilt: [3, 4], color: 5, gobo: 6, strobe: 7, dim: 8,
                speed: 9, mode: 10, strip: 12 },
  // The same light in 10-channel mode: 8-bit pan/tilt
  miniHead10: { pan: 1, tilt: 2, color: 3, gobo: 4, strobe: 5, dim: 6, speed: 7, mode: 8,
                strip: 10 },
  // 13-channel moving head (the channel names 1-13 in the console)
  head13: { dim: 1, strobe: 2, pan: [3, 4], tilt: [5, 6], speed: 7, color: 8, gobo: 9,
            prism: 10, mode: 11, autoSpeed: 12, reset: 13 },
};

// Colour wheel positions (miniHead colour channel 0-139) and LED ring colours (strip 5-109),
// for effects that step through colours. Tune them to your fixture.
var WHEEL = [0, 20, 40, 60, 80, 100, 120];
var RING  = [10, 25, 40, 55, 70, 85, 100];

// The rig: one line per light, at its DMX start address.
var heads = [
  fixture(1, TYPES.miniHead12),
  fixture(13, TYPES.miniHead12),
  fixture(25, TYPES.miniHead12),
  fixture(37, TYPES.miniHead12),
];

// Put the heads under DMX control: no built-in program, no strobe, fastest pan/tilt so they
// follow the positions an effect streams. Effects call it every frame.
function ready(list) {
  (list || heads).forEach(function (h) {
    h.mode = 0;
    h.speed = 0;
    h.strobe = 0;
  });
}
