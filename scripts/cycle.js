// Cycle: plays other effects in turn, each for a while, then the next one. Edit the list to
// choose them; a cycle can play other cycles too. Each effect keeps its own sliders (shown as
// "fan-circle.speed" etc.) and picks up where it left off on its next turn.
var effects = [effect('fan-circle'), effect('figure-eight'), effect('color-chase')];
var current = -1, left = 0;
var clock = effects.map(function () { return 0; });   // each effect's own running time

function frame(t, dt) {
  var seconds = param('seconds', 20, 2, 300, 1);   // how long each effect runs
  left -= dt;
  if (left <= 0) {
    current = (current + 1) % effects.length;
    left = seconds;
    print('cycle:', effects[current].name);
  }
  clock[current] += dt;
  effects[current].frame(clock[current], dt);
}
