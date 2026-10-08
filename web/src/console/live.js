import { useEffect, useReducer } from 'preact/hooks';

/*
 * Live console state shared with the bridge over the /ws WebSocket. Kept outside Preact state
 * because it changes ~15 times a second for all 512 channels; components re-render through
 * useLive(), which bumps on every change.
 *
 * Protocol (see main/console.c):
 *   client -> bridge  binary [ch_hi, ch_lo, value]*  (ch 0xFFFF = master), text "clear"
 *   bridge -> client  binary [0x01][master][manual x512][output x512], text "names" / "scenes"
 */
export const N = 512;
export const MASTER = 0xFFFF;

const manual = new Uint8Array(N);
const output = new Uint8Array(N);
let master = 255;
let connected = false;

const touched = new Map();   // ch -> time of the last local change (ignore stale echoes briefly)
const held = new Set();      // channels held by a gesture (drag, flash): frames don't move them
const pending = new Map();   // ch -> value not sent yet
const changeListeners = new Set();
const eventListeners = new Set();
let ws = null;
let sendTimer = null;
let lastSend = 0;

const notify = () => changeListeners.forEach(fn => fn());
const emit = ev => eventListeners.forEach(fn => fn(ev));

export const getValue = ch => (ch === MASTER ? master : manual[ch]);
export const getOutput = ch => output[ch];
export const isConnected = () => connected;

export function setValue(ch, v) {
  v = Math.max(0, Math.min(255, Math.round(v)));
  if (ch === MASTER) master = v; else manual[ch] = v;
  touched.set(ch, performance.now());
  pending.set(ch, v);
  scheduleSend();
  notify();
}

export function hold(ch, on) {
  if (on) held.add(ch); else held.delete(ch);
}

// Set every console fader to 0 (the bridge also cancels a running scene fade).
export function clearAll() {
  manual.fill(0);
  pending.clear();
  const now = performance.now();
  for (let i = 0; i < N; i++) touched.set(i, now);
  if (ws && ws.readyState === 1) ws.send('clear');
  notify();
}

// Re-render the calling component on every live change.
export function useLive() {
  const [, bump] = useReducer(x => x + 1, 0);
  useEffect(() => {
    changeListeners.add(bump);
    return () => changeListeners.delete(bump);
  }, []);
}

// Called with 'open' (connected), 'names' or 'scenes' (the bridge's copy changed).
export function useLiveEvents(fn) {
  useEffect(() => {
    eventListeners.add(fn);
    return () => eventListeners.delete(fn);
  }, [fn]);
}

function scheduleSend() {
  if (sendTimer) return;
  sendTimer = setTimeout(flush, Math.max(0, 25 - (performance.now() - lastSend)));
}

function flush() {
  sendTimer = null;
  if (!ws || ws.readyState !== 1 || pending.size === 0) return;
  const buf = new Uint8Array(pending.size * 3);
  let i = 0;
  for (const [ch, v] of pending) {
    buf[i++] = ch >> 8;
    buf[i++] = ch & 255;
    buf[i++] = v;
  }
  pending.clear();
  ws.send(buf);
  lastSend = performance.now();
}

function onFrame(d) {
  if (d[0] !== 1 || d.length < 2 + 2 * N) return;
  const now = performance.now();
  const keep = ch => held.has(ch) || now - (touched.get(ch) ?? -1e9) < 400;
  if (!keep(MASTER)) master = d[1];
  for (let i = 0; i < N; i++) {
    if (!keep(i)) manual[i] = d[2 + i];
    output[i] = d[2 + N + i];
  }
  notify();
}

export function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => {
    connected = true;
    flush();
    notify();
    emit('open');
  };
  ws.onclose = () => {
    connected = false;
    notify();
    setTimeout(connect, 1500);
  };
  ws.onerror = () => ws.close();
  ws.onmessage = ev => {
    if (typeof ev.data === 'string') emit(ev.data);
    else onFrame(new Uint8Array(ev.data));
  };
}
