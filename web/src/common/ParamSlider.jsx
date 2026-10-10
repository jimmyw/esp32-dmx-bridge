import { useEffect, useRef, useState } from 'preact/hooks';
import { setParams } from './scripts';

const SEND_MS = 80;

function fmt(v, step) {
  if (step >= 1) return String(Math.round(v));
  const a = Math.abs(v);
  return a >= 100 ? v.toFixed(0) : a >= 10 ? v.toFixed(1) : v.toFixed(2);
}

// A live script parameter ({name, value, min, max, step} from /api/scripts). Sends while dragging,
// at most every SEND_MS.
export function ParamSlider({ p }) {
  const [v, setV] = useState(p.value);
  const dragging = useRef(false);
  const timer = useRef(0);
  const pending = useRef(null);
  useEffect(() => { if (!dragging.current) setV(p.value); }, [p.value]);
  useEffect(() => () => clearTimeout(timer.current), []);

  const flush = () => {
    timer.current = 0;
    if (pending.current === null) return;
    setParams({ [p.name]: pending.current }).catch(() => {});
    pending.current = null;
  };
  const send = x => {
    pending.current = x;
    if (!timer.current) timer.current = setTimeout(flush, SEND_MS);
  };
  const step = p.step > 0 ? p.step : (p.max - p.min) / 200 || 0.01;

  return (
    <label class="param" title={`${p.name}: ${p.min} – ${p.max}`}>
      <span class="pn">{p.label || p.name}</span>
      <input type="range" min={p.min} max={p.max} step={step} value={v}
             onPointerDown={() => { dragging.current = true; }}
             onInput={e => { const x = +e.currentTarget.value; setV(x); send(x); }}
             onChange={() => { dragging.current = false; }} />
      <span class="pv">{fmt(v, p.step)}</span>
    </label>
  );
}
