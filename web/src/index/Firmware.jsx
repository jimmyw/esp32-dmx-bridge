import { useRef, useState } from 'preact/hooks';

// Firmware upload: POSTs the raw .bin to /api/ota with progress, then reloads after the restart.
export function Firmware({ status }) {
  const file = useRef();
  const [busy, setBusy] = useState(false);
  const [progress, setProgress] = useState(null);   // null = hidden, else 0..100
  const [msg, setMsg] = useState({ text: '', bad: false });
  const say = (text, bad = false) => setMsg({ text, bad });

  const upload = () => {
    const f = file.current.files[0];
    if (!f) return say('Choose a .bin file first', true);
    if (!f.name.endsWith('.bin')) return say('Expected a .bin firmware file', true);
    const x = new XMLHttpRequest();
    x.open('POST', '/api/ota');
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
      if (x.status === 200 && r.ok) {
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
        {status ? `Running v${status.fw} from partition ${status.partition}` : ''}
      </div>
      <div class="row" style={{ marginBottom: '6px' }}>
        <div><input ref={file} type="file" accept=".bin,application/octet-stream" /></div>
      </div>
      <div class="actions">
        <button type="button" class="sec" disabled={busy} onClick={upload}>Upload &amp; install</button>
        {progress !== null && <progress max="100" value={progress} style={{ flex: 1, minWidth: '120px' }} />}
        <span class="hint" style={{ color: msg.bad ? 'var(--bad)' : '' }}>{msg.text}</span>
      </div>
      <div class="hint">Select <code>build/dmx_bridge.bin</code>. The bridge restarts into the new firmware and
        rolls back automatically if it fails to start. DMX output may stutter during the upload.</div>
    </section>
  );
}
