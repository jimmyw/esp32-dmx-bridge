import { useRef, useState } from 'preact/hooks';
import { MASTER, getOutput, getValue, hold, setValue } from './live';

export const fmt = (v, pct) => (pct ? Math.round(v / 2.55) + '%' : String(v));

// Positions inside the track: 10px end stops, the rest is the 0..255 travel.
const travel = f => `calc((100% - 20px) * ${f})`;

function Track({ ch, meter }) {
  const ref = useRef();
  const grab = useRef(null);   // pointer offset from the cap centre while dragging
  const v = getValue(ch);

  const fromY = y => {
    const r = ref.current.getBoundingClientRect();
    return (1 - (y - grab.current - r.top - 10) / Math.max(1, r.height - 20)) * 255;
  };
  const down = e => {
    ref.current.setPointerCapture(e.pointerId);
    const r = ref.current.getBoundingClientRect();
    const capY = r.top + 10 + (1 - getValue(ch) / 255) * (r.height - 20);
    grab.current = Math.abs(e.clientY - capY) < 16 ? e.clientY - capY : 0;   // cap: relative, else jump
    hold(ch, true);
    setValue(ch, fromY(e.clientY));
  };
  const move = e => { if (grab.current !== null) setValue(ch, fromY(e.clientY)); };
  const up = () => { grab.current = null; hold(ch, false); };
  const wheel = e => {
    e.preventDefault();
    setValue(ch, getValue(ch) + (e.deltaY < 0 ? 1 : -1) * (e.shiftKey ? 10 : 1));
  };

  return (
    <div class="track" ref={ref} onPointerDown={down} onPointerMove={move} onPointerUp={up}
         onPointerCancel={up} onWheel={wheel}>
      <div class="slot" />
      <div class="fill" style={{ height: travel(v / 255) }} />
      {meter && <div class="meter" style={{ height: travel(getOutput(ch) / 255) }} />}
      <div class="cap" style={{ top: `calc(10px + ${travel(1 - v / 255)})` }} />
    </div>
  );
}

// Momentary button: holds `value` while pressed, then restores the fader.
function Momentary({ ch, value, children }) {
  const [down, setDown] = useState(false);
  const saved = useRef(0);
  const press = e => {
    e.preventDefault();
    saved.current = getValue(ch);
    setDown(true);
    hold(ch, true);
    setValue(ch, value);
  };
  const release = () => {
    if (!down) return;
    setDown(false);
    hold(ch, false);
    setValue(ch, saved.current);
  };
  return (
    <button class={'fl' + (down ? ' down' : '')} onPointerDown={press} onPointerUp={release}
            onPointerLeave={release} onPointerCancel={release}>{children}</button>
  );
}

export function Strip({ ch, name, hidden, pct, onMenu }) {
  if (ch < 0) return <div class="strip blank" />;   // filler at the end of the last bank
  const title = (name ? `Ch ${ch + 1}: ${name}` : `Ch ${ch + 1}`) +
    (hidden ? (name ? ' (hidden)' : ' (hidden: no name)') : '');
  return (
    <div class={'strip' + (hidden ? ' hid' : '')} title={title} data-ch={ch}>
      <div class={'nm' + (name ? '' : ' empty')} onClick={() => onMenu(ch)}>{name || 'name'}</div>
      <div class="val">{fmt(getValue(ch), pct)}</div>
      <Track ch={ch} meter />
      <div class="ch">{ch + 1}</div>
      <div class="btns">
        <Momentary ch={ch} value={255}>Flash</Momentary>
        <button class="zero" onClick={() => setValue(ch, 0)}>0</button>
      </div>
    </div>
  );
}

export function MasterStrip({ pct }) {
  return (
    <div class="strip master">
      <div class="nm">MASTER</div>
      <div class="val">{fmt(getValue(MASTER), pct)}</div>
      <Track ch={MASTER} />
      <div class="ch">Master</div>
      <div class="btns">
        <Momentary ch={MASTER} value={0}>DBO</Momentary>
        <button class="zero" onClick={() => setValue(MASTER, 255)}>Full</button>
      </div>
    </div>
  );
}
