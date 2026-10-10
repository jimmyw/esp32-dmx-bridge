import { ParamSlider } from './ParamSlider';

// "fan-circle" -> "Fan circle"
export const label = n => { const s = n.replace(/[-_]+/g, ' ').trim(); return s.charAt(0).toUpperCase() + s.slice(1); };

/*
 * The running script's parameters. Children loaded with effect() name theirs "<child>.<name>"
 * ("cycle" -> "fan-circle.speed"): those are grouped under the child's name, after the script's
 * own. ownOnly shows just the script's own ones.
 */
export function ParamList({ params, running, ownOnly = false }) {
  const groups = new Map([['', []]]);
  for (const p of params) {
    const dot = p.name.lastIndexOf('.');
    const g = dot < 0 ? '' : p.name.slice(0, dot);
    if (!groups.has(g)) groups.set(g, []);
    groups.get(g).push({ ...p, label: p.name.slice(dot + 1) });
  }
  const shown = [...groups].filter(([g, ps]) => ps.length && (!ownOnly || g === ''));
  return (
    <div class="param-groups">
      {shown.map(([g, ps]) => (
        <div key={g} class="param-group">
          {g && <h3 class="pg">{g.split('.').map(label).join(' › ')}</h3>}
          <div class="params">
            {ps.map(p => <ParamSlider key={running + '/' + p.name} p={p} />)}
          </div>
        </div>
      ))}
    </div>
  );
}
