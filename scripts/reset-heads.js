// Reset: holds channel 11 of every head at 255 for 6 s (the mini head's reset: 250-255 for
// 5 s), dark, so they re-home pan and tilt. Run it when the heads no longer line up, then start
// an effect again. Channel 11 is the mini head's reset in 12-channel mode: change it for others.
include('setup');

function frame(t, dt) {
  heads.forEach(function (h) {
    set(h.address + 10, t < 6 ? 255 : 0);
    h.dim = 0;
  });
}
