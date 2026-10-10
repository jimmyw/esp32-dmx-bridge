import { useEffect, useState } from 'preact/hooks';
import { getJson, postJson } from '../common/api';
import { useInterval } from '../common/hooks';
import { usePersisted } from '../common/persisted';
import { ParamList, label } from '../common/ParamList';
import { runScript, stopScript } from '../common/scripts';
import { Status, useStatus } from '../common/Status';

/*
 * Start page: one big button per effect script (tap to run, tap the running one to stop; its
 * live parameters show below) and one per stored scene (tap to recall it, with the console's fade
 * time). Settings moved to /settings.
 */


export function App() {
  const [st, setSt] = useState(null);
  const [status, statusOffline] = useStatus();      // polls /api/status every second
  const [names, setNames] = useState({});
  const [offline, setOffline] = useState(false);
  const [busy, setBusy] = useState('');          // script being started/stopped
  const [msg, setMsg] = useState('');
  const [scenes, setScenes] = useState(null);
  const [fade, setFade] = usePersisted('fade', 0);   // shared with the console's scene bar

  const refresh = async () => {
    try {
      const [s, sc] = await Promise.all([getJson('/api/scripts'), getJson('/api/scenes')]);
      setSt(s);
      setScenes(sc);
      setOffline(false);
    } catch (e) { setOffline(true); }
  };
  useInterval(refresh, 1500);
  useEffect(() => { getJson('/api/names').then(r => setNames(r.names)).catch(() => {}); }, []);

  const name = status ? status.name : 'DMX Bridge';
  useEffect(() => { document.title = name; }, [name]);

  const act = async (target, fn) => {
    if (busy) return;
    setBusy(target);
    setMsg('');
    try {
      const r = await fn();
      setSt(r);
      if (!r.ok) setMsg(r.error || 'failed');
    } catch (e) { setMsg('connection failed'); }
    setBusy('');
  };
  const toggle = n => act(n, () => (st.running === n ? stopScript() : runScript(n)));

  const recall = async id => {
    const f = Math.max(0, Math.min(60, parseFloat(fade) || 0));
    setScenes(sc => ({ ...sc, active: id, fading: f > 0 }));
    setMsg('');
    try {
      const r = await postJson('/api/scenes', { action: 'recall', id, fade_ms: Math.round(f * 1000) });
      if (!r.ok) setMsg(r.error || 'recall failed');
      setScenes(await getJson('/api/scenes'));
    } catch (e) { setMsg('connection failed'); }
  };

  const scripts = st ? st.scripts.filter(s => !s.lib).sort((a, b) => a.name.localeCompare(b.name)) : [];   // not setup.js
  const running = st && st.running;
  const failed = st && st.failed;
  const setupMode = status && !status.wifi.sta && status.wifi.ap;

  return (
    <main class="board">
      <header>
        <h1>{name}</h1>
        <i class={'dot' + (status && status.dmx.signal ? ' on' : '')}
           title={status && status.dmx.signal ? 'receiving DMX from the network' : 'no network DMX input'} />
        <span class="sp" />
        <nav>
          <a href="/console"><button type="button" class="sec">Console</button></a>
          <a href="/scripts"><button type="button" class="sec">Scripts</button></a>
          <a href="/settings"><button type="button" class="sec">Settings</button></a>
        </nav>
      </header>

      {setupMode && (
        <a class="banner" href="/settings">Not on Wi-Fi yet – open <b>Settings</b> to connect the bridge.</a>
      )}
      {(offline || statusOffline) && <div class="banner bad">Bridge not reachable – retrying…</div>}

      <h2 class="sec-h">Effects</h2>
      <div class="tiles">
        {scripts.map(s => {
          const on = s.name === running, bad = failed && failed.name === s.name;
          return (
            <button key={s.name} type="button"
                    class={'tile' + (on ? ' on' : '') + (bad ? ' bad' : '') + (busy === s.name ? ' busy' : '')}
                    aria-pressed={on} title={on ? 'running – tap to stop' : bad ? failed.error : 'tap to run'}
                    onClick={() => toggle(s.name)}>
              <span class="tl">{label(s.name)}</span>
              <span class="ts">{on ? 'Running' : bad ? 'Error' : busy === s.name ? '…' : ''}</span>
            </button>
          );
        })}
      </div>
      {st && !scripts.length && (
        <p class="empty">No effect scripts yet. Write one on the <a href="/scripts">Scripts</a> page.</p>
      )}

      {failed && <div class="err">{label(failed.name)}: {failed.error}</div>}
      {msg && <div class="err">{msg}</div>}

      {running && (
        <section class="card params-card">
          <div class="ph">
            <h2>{label(running)}</h2>
            <span class="sp" />
            <a href={`/scripts?name=${encodeURIComponent(running)}`}><button type="button" class="sec small">Edit</button></a>
            <button type="button" class="danger small" disabled={!!busy}
                    onClick={() => act(running, stopScript)}>Stop</button>
          </div>
          {st.params.length > 0 ? (
            <ParamList params={st.params} running={running} />
          ) : <p class="hint">This script has no parameters.</p>}
        </section>
      )}

      <div class="sec-row">
        <h2 class="sec-h">Scenes</h2>
        <span class="sp" />
        <label class="fade">Fade
          <input type="number" min="0" max="60" step="0.1" value={fade}
                 onChange={e => setFade(e.currentTarget.value)} /> s</label>
      </div>
      {scenes && scenes.scenes.length > 0 ? (
        <div class="tiles scenes">
          {scenes.scenes.map(sc => {
            const on = sc.id === scenes.active;
            return (
              <button key={sc.id} type="button"
                      class={'tile scene' + (on ? ' on' : '') + (on && scenes.fading ? ' fading' : '')}
                      aria-pressed={on} title={`recall scene ${sc.id}`} onClick={() => recall(sc.id)}>
                <span class="tl">{sc.name}</span>
                <span class="ts">{on ? (scenes.fading ? 'Fading…' : 'Active') : ''}</span>
              </button>
            );
          })}
        </div>
      ) : scenes && (
        <p class="empty">No scenes yet. Set the faders in the <a href="/console">Console</a> and press
          <b> + Save</b> on its scene bar.</p>
      )}

      <div class="status-wrap">
        <Status status={status} offline={statusOffline} names={names} />
      </div>
    </main>
  );
}
