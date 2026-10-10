import { useCallback, useState } from 'preact/hooks';
import { getJson } from '../common/api';
import { ParamSlider } from '../common/ParamSlider';
import { runScript, stopScript } from '../common/scripts';
import { useLiveEvents } from './live';

/*
 * Effect scripts toolbar: one button per script (tap to run, tap again to stop) and the running
 * script's live parameters. Scripts are edited on /scripts.
 */
export function ScriptBar() {
  const [st, setSt] = useState(null);
  const load = useCallback(async () => {
    try { setSt(await getJson('/api/scripts')); } catch (e) {}
  }, []);
  useLiveEvents(useCallback(ev => { if (ev === 'open' || ev === 'scripts') load(); }, [load]));

  if (!st) return null;
  const toggle = async name => {
    try { setSt(await (st.running === name ? stopScript() : runScript(name))); } catch (e) {}
  };
  const scripts = st.scripts.filter(s => !s.lib).sort((a, b) => a.name.localeCompare(b.name));   // not setup.js

  return (
    <div id="fxbar">
      <div class="fxrow">
        <span class="lbl">FX</span>
        <div id="fx">
          {scripts.map(s => (
            <button key={s.name} class={s.name === st.running ? 'active' : ''}
                    title={s.name === st.running ? 'running – tap to stop' : 'run this script'}
                    onClick={() => toggle(s.name)}>{s.name}</button>
          ))}
          {!scripts.length && <span class="lbl">no scripts yet</span>}
        </div>
        <a class="btn" href="/scripts" title="write and edit effect scripts">Edit</a>
      </div>
      {st.failed && <div class="fxerr">{st.failed.name}: {st.failed.error}</div>}
      {st.running && st.params.length > 0 && (
        <div id="fxparams">
          {st.params.map(p => <ParamSlider key={st.running + '/' + p.name} p={p} />)}
        </div>
      )}
    </div>
  );
}
