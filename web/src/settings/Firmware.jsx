import { useRef, useState } from 'preact/hooks';

// What the bridge serves the web pages from, e.g. "web UI 1.0.0 (a58dd5f, 2026-10-08)".
function webInfo(w) {
  if (!w) return '';
  if (w.source !== 'package') return 'web UI built into the firmware';
  const extra = [w.git, w.built && w.built.slice(0, 10)].filter(Boolean).join(', ');
  return `web UI ${w.version || '?'}` + (extra ? ` (${extra})` : '');
}

/*
 * Upload either file from the build directory:
 *   dmx_bridge.bin       firmware -> /api/ota, the bridge restarts into it
 *   dmx_bridge_www.tar   web UI only -> /api/www, installed without a restart
 */
export function Firmware({ status }) {
  const file = useRef();
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState(null);   // null = hidden, else 0..100
  const [msg, setMsg] = useState({ text: '', bad: false });
  const say = (text, bad = false) => setMsg({ text, bad });

  const upload = () => {
    const f = file.current.files[0];
    if (!f) return say('Choose a file first', true);
    const web = f.name.endsWith('.tar');
    if (!web && !f.name.endsWith('.bin')) return say('Expected dmx_bridge.bin or dmx_bridge_www.tar', true);
    const x = new XMLHttpRequest();
    x.open('POST', web ? '/api/www' : '/api/ota');
    x.setRequestHeader('Content-Type', 'application/octet-stream');
    x.upload.onprogress = e => {
      if (!e.lengthComputable) return;
      const p = (e.loaded / e.total) * 100;
      setProgress(p);
      say(e.loaded < e.total ? `Uploading ${Math.round(p)}%` : 'Verifying…');
    };
    x.onload = () => {
      setBusy(false);
      let r = {};
      try { r = JSON.parse(x.responseText); } catch (e) {}
      if (x.status === 200 && r.ok && web) {
        say(`Web UI installed (${r.files} files) – reloading…`);
        setTimeout(() => location.reload(), 1500);
      } else if (x.status === 200 && r.ok) {
        say('Installed – restarting…');
        setTimeout(() => location.reload(), 10000);
      } else {
        setProgress(null);
        say(r.error || `Upload failed (HTTP ${x.status})`, true);
      }
    };
    x.onerror = () => { setBusy(false); setProgress(null); say('Upload failed (connection lost)', true); };
    setBusy(true);
    setProgress(0);
    say('Uploading…');
    x.send(f);
  };

  return (
    <section class="card">
      <h2>Firmware</h2>
      <div class="hint" style={{ marginBottom: '8px' }}>
        {status ? `Running v${status.fw} from partition ${status.partition} · ${webInfo(status.web)}` : ''}
      </div>
      <div class="row" style={{ marginBottom: '6px' }}>
        <div><input ref={file} type="file" accept=".bin,.tar,application/octet-stream,application/x-tar" /></div>
      </div>
      <div class="actions">
        <button type="button" class="sec" disabled={busy} onClick={upload}>Upload &amp; install</button>
        {progress !== null && <progress max="100" value={progress} style={{ flex: 1, minWidth: '120px' }} />}
        <span class="hint" style={{ color: msg.bad ? 'var(--bad)' : '' }}>{msg.text}</span>
      </div>
      <div class="hint">Select <code>build/dmx_bridge.bin</code> to update the firmware: the bridge restarts
        into it and rolls back automatically if it fails to start. Or select <code>build/dmx_bridge_www.tar</code>
        to update only the web pages, without a restart. DMX output may stutter during an upload.
        If an uploaded web UI breaks, open <a href="/settings?builtin">/settings?builtin</a> for the firmware's own copy.</div>
    </section>
  );
}
