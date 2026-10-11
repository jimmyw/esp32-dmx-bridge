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
    mic: !!(d[1] & 1), signal: !!(d[1] & 4), locked: !!(d[1] & 8),
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

// A slider for one config value, saved when let go. fmt(v) -> [value text, hint text];
// fromCfg/toCfg map between the stored value and the slider's.
const same = v => v;
function ConfigSlider({ name, label, min, max, step, fmt, reset, title, config, fromCfg = same, toCfg = same }) {
  const [v, setV] = useState(null);
  const [msg, setMsg] = useState('');
  useEffect(() => { if (config && v === null) setV(fromCfg(config[name] ?? reset)); }, [config]);
  if (v === null) return null;
  const save = async x => {
    try {
      const r = await postJson('/api/config', { [name]: toCfg(x) });
      setMsg(r.ok ? '' : r.error || 'save failed');
    } catch (e) { setMsg('save failed'); }
  };
  const [text, hint] = fmt(v);
  return (
    <label class="cslider" title={title}>
      <span class="mn">{label}</span>
      <input type="range" min={min} max={max} step={step} value={v}
             onInput={e => setV(+e.currentTarget.value)} onChange={e => save(+e.currentTarget.value)} />
      <span class="ov">{text}</span>
      <span class="hint">{msg || hint}</span>
      {/* always rendered, so its slot doesn't come and go while dragging */}
      <button type="button" class="sec small reset" style={{ visibility: v !== reset ? 'visible' : 'hidden' }}
              title={`back to ${reset}`} onClick={() => { setV(reset); save(reset); }}>{reset}</button>
    </label>
  );
}

// Offset: shifts the whole beat grid, so lights that react late can be pulled onto the music.
const offsetFmt = v => [`${v > 0 ? '+' : ''}${v} ms`, v > 0 ? 'beats earlier' : v < 0 ? 'beats later' : 'no shift'];
// Fallback tempo: effect scripts get a timed beat at this tempo when the microphone gives none.
// Left end = off: then scripts get no beat without music.
const fallbackFmt = v => (v < 60 ? ['off', 'no beat without music'] : [`${v} BPM`, 'scripts, without a beat']);

export function Audio() {
  const frame = useAudioFrames();
  const [info, setInfo] = useState(null);   // /api/audio: mic_data (is the mic sending?)
  const [config, setConfig] = useState(null);
  useEffect(() => { getJson('/api/config').then(setConfig).catch(() => {}); }, []);
  useInterval(async () => { try { setInfo(await getJson('/api/audio')); } catch (e) {} }, 3000);

  const f = frame || { mic: false, signal: false, level: 0, bass: 0, mid: 0, high: 0, bpm: 0, phase: 0, db: -100 };

  let state;
  if (!f.mic) state = 'No microphone – set its pins in Settings';
  else if (info && !info.mic_data) state = 'Microphone sends no data – check the wiring';
  else if (!f.signal) state = `Quiet (${f.db.toFixed(0)} dBFS, below the noise gate)`;
  else state = `Listening · ${f.db.toFixed(0)} dBFS`;

  return (
    <section class="card audio">
      <div class="ph">
        <h2>Sound</h2>
        <span class="hint astate">{state}</span>
      </div>
      <Spectrum frame={frame} />
      <div class="audio-row">
        <div class="meters">
          <Meter name="Level" v={f.level} />
          <Meter name="Bass" v={f.bass} />
          <Meter name="Mid" v={f.mid} />
          <Meter name="High" v={f.high} />
        </div>
        <div class="tempo" title="tempo from a steady kick; the ring fills once per beat. Without a kick: no beat, flashes on the notes">
          <PhaseRing phase={f.phase} on={f.bpm > 0} />
          <span class="bpm">{f.bpm > 0 ? Math.round(f.bpm) : '–'}
            <small>{f.bpm > 0 ? 'BPM' : f.signal ? 'no beat' : 'BPM'}</small></span>
        </div>
      </div>
      <ConfigSlider name="mic_gate" label="Gate" min={-90} max={-30} step={1} reset={-62}
                    fmt={v => [`${v} dBFS`, `now ${f.db.toFixed(0)} dBFS${f.signal ? '' : ' (below)'}`]}
                    config={config} fromCfg={c => -c} toCfg={x => -x}
                    title="sound quieter than this counts as silence: set it above the room's noise and below the music" />
      <ConfigSlider name="beat_offset_ms" label="Offset" min={-300} max={300} step={5} reset={0} fmt={offsetFmt}
                    config={config} title="move the beat earlier (+) to make up for lights that react late, or later (-)" />
      <ConfigSlider name="fallback_bpm" label="Fallback" min={55} max={180} step={1} reset={120} fmt={fallbackFmt}
                    config={config} fromCfg={c => (c ? c : 55)} toCfg={x => (x < 60 ? 0 : x)}
                    title="effect scripts get a timed beat at this tempo when the microphone gives no beat (left end = off)" />
    </section>
  );
}
