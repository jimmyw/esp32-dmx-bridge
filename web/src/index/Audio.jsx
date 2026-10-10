import { useEffect, useRef, useState } from 'preact/hooks';
import { getJson, postJson } from '../common/api';
import { useInterval } from '../common/hooks';

/*
 * Sound card: live spectrum, level/bass/mid/high meters, beat lamp, tempo and beat phase, from
 * the microphone analysis (main/audio.c). Frames arrive over /ws at ~15/s after subscribing with
 * "audio" (repeated, the subscription expires); drawing runs at the display rate and eases
 * between frames.
 */
const BANDS = 16, SPECTRUM = 32;

function parse(d) {
  if (d[0] !== 2 || d.length < 12 + BANDS + SPECTRUM) return null;
  return {
    mic: !!(d[1] & 1), demo: !!(d[1] & 2), signal: !!(d[1] & 4),
    level: d[2] / 255, bass: d[3] / 255, mid: d[4] / 255, high: d[5] / 255,
    beats: d[6] | (d[7] << 8), bpm: (d[8] | (d[9] << 8)) / 10, phase: d[10] / 255, db: d[11] - 100,
    bands: d.slice(12, 12 + BANDS), spectrum: d.slice(12 + BANDS, 12 + BANDS + SPECTRUM),
  };
}

// Live audio frames while mounted; null until the first one.
function useAudioFrames() {
  const [frame, setFrame] = useState(null);
  useEffect(() => {
    let ws, timer, retry, gone = false;
    const open = () => {
      ws = new WebSocket(`ws://${location.host}/ws`);
      ws.binaryType = 'arraybuffer';
      ws.onopen = () => {
        ws.send('audio');
        timer = setInterval(() => ws.readyState === 1 && ws.send('audio'), 2000);
      };
      ws.onmessage = ev => {
        if (typeof ev.data === 'string') return;
        const f = parse(new Uint8Array(ev.data));
        if (f) setFrame(f);
      };
      ws.onclose = () => {
        clearInterval(timer);
        if (!gone) retry = setTimeout(open, 2000);
      };
    };
    open();
    return () => { gone = true; clearInterval(timer); clearTimeout(retry); ws && ws.close(); };
  }, []);
  return frame;
}

function Spectrum({ frame }) {
  const canvas = useRef();
  const target = useRef(new Float32Array(SPECTRUM));
  const shown = useRef(new Float32Array(SPECTRUM));
  const beatAt = useRef(0);
  const lastBeats = useRef(-1);

  if (frame) {
    for (let i = 0; i < SPECTRUM; i++) target.current[i] = frame.spectrum[i] / 255;
    if (lastBeats.current >= 0 && frame.beats !== lastBeats.current) beatAt.current = performance.now();
    lastBeats.current = frame.beats;
  }

  useEffect(() => {
    let raf;
    const draw = () => {
      raf = requestAnimationFrame(draw);
      const c = canvas.current;
      if (!c) return;
      const dpr = devicePixelRatio || 1, w = c.clientWidth, h = c.clientHeight;
      if (c.width !== Math.round(w * dpr) || c.height !== Math.round(h * dpr)) {
        c.width = Math.round(w * dpr);
        c.height = Math.round(h * dpr);
      }
      const g = c.getContext('2d');
      g.setTransform(dpr, 0, 0, dpr, 0, 0);
      g.clearRect(0, 0, w, h);
      // Beat: the background flashes and fades over 250 ms.
      const flash = Math.max(0, 1 - (performance.now() - beatAt.current) / 250);
      if (flash > 0) {
        g.fillStyle = `rgba(240, 166, 58, ${0.22 * flash})`;
        g.fillRect(0, 0, w, h);
      }
      const gap = 2, bw = (w - gap * (SPECTRUM - 1)) / SPECTRUM;
      for (let i = 0; i < SPECTRUM; i++) {
        const s = shown.current;
        s[i] += (target.current[i] - s[i]) * 0.35;   // ease between ~15/s frames
        const bh = Math.max(1, s[i] * (h - 4));
        g.fillStyle = `hsl(${260 - (i / (SPECTRUM - 1)) * 260}, 75%, ${50 + 10 * s[i]}%)`;
        g.fillRect(i * (bw + gap), h - bh, bw, bh);
      }
    };
    draw();
    return () => cancelAnimationFrame(raf);
  }, []);

  return <canvas ref={canvas} class="spectrum" aria-label="spectrum" />;
}

function Meter({ name, v }) {
  return (
    <div class="meter-row">
      <span class="mn">{name}</span>
      <span class="mbar"><i style={{ width: `${Math.round(v * 100)}%` }} /></span>
    </div>
  );
}

// Beat phase as a ring that fills once per beat.
function PhaseRing({ phase, on }) {
  const r = 15, c = 2 * Math.PI * r;
  return (
    <svg class="ring" viewBox="0 0 40 40" aria-label="beat phase">
      <circle cx="20" cy="20" r={r} class="rbg" />
      {on && <circle cx="20" cy="20" r={r} class="rfg" stroke-dasharray={`${c * phase} ${c}`}
                     transform="rotate(-90 20 20)" />}
    </svg>
  );
}

// Beat offset (config beat_offset_ms): shifts the whole beat grid, so lights that react late can
// be pulled onto the music. Saved when the slider is let go.
function OffsetSlider() {
  const [v, setV] = useState(null);
  const [msg, setMsg] = useState('');
  useEffect(() => { getJson('/api/config').then(c => setV(c.beat_offset_ms ?? 0)).catch(() => {}); }, []);
  if (v === null) return null;
  const save = async x => {
    try {
      const r = await postJson('/api/config', { beat_offset_ms: x });
      setMsg(r.ok ? '' : r.error || 'save failed');
    } catch (e) { setMsg('save failed'); }
  };
  return (
    <label class="offset" title="move the beat earlier (+) to make up for lights that react late, or later (-)">
      <span class="mn">Offset</span>
      <input type="range" min="-300" max="300" step="5" value={v}
             onInput={e => setV(+e.currentTarget.value)} onChange={e => save(+e.currentTarget.value)} />
      <span class="ov">{v > 0 ? '+' : ''}{v} ms</span>
      <span class="hint">{msg || (v > 0 ? 'beats earlier' : v < 0 ? 'beats later' : 'no shift')}</span>
      {v !== 0 && <button type="button" class="sec small" onClick={() => { setV(0); save(0); }}>0</button>}
    </label>
  );
}

export function Audio() {
  const frame = useAudioFrames();
  const [info, setInfo] = useState(null);   // /api/audio: mic_data (is the mic sending?)
  const [busy, setBusy] = useState(false);
  useInterval(async () => { try { setInfo(await getJson('/api/audio')); } catch (e) {} }, 3000);

  const f = frame || { mic: false, demo: false, signal: false, level: 0, bass: 0, mid: 0, high: 0, bpm: 0, phase: 0, db: -100 };
  const demo = async on => {
    setBusy(true);
    try { setInfo(await postJson('/api/audio', { demo: on })); } catch (e) {}
    setBusy(false);
  };

  let state;
  if (f.demo) state = 'Demo track (120 BPM)';
  else if (!f.mic) state = 'No microphone – set its pins in Settings';
  else if (info && !info.mic_data) state = 'Microphone sends no data – check the wiring';
  else if (!f.signal) state = `Quiet (${f.db.toFixed(0)} dBFS, below the noise gate)`;
  else state = `Listening · ${f.db.toFixed(0)} dBFS`;

  return (
    <section class="card audio">
      <div class="ph">
        <h2>Sound</h2>
        <span class="hint astate">{state}</span>
        <span class="sp" />
        <button type="button" class={'sec small' + (f.demo ? ' on' : '')} disabled={busy}
                title="a synthesized 120 BPM track instead of the microphone, to try effects"
                onClick={() => demo(!f.demo)}>{f.demo ? 'Stop demo' : 'Demo'}</button>
      </div>
      <Spectrum frame={frame} />
      <div class="audio-row">
        <div class="meters">
          <Meter name="Level" v={f.level} />
          <Meter name="Bass" v={f.bass} />
          <Meter name="Mid" v={f.mid} />
          <Meter name="High" v={f.high} />
        </div>
        <div class="tempo" title="tempo from the beats; the ring fills once per beat">
          <PhaseRing phase={f.phase} on={f.bpm > 0} />
          <span class="bpm">{f.bpm > 0 ? Math.round(f.bpm) : '–'}<small>BPM</small></span>
        </div>
      </div>
      <OffsetSlider />
    </section>
  );
}
