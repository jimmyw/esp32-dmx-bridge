import { useEffect, useRef, useState } from 'preact/hooks';
import { getJson } from '../common/api';
import { useConfirm, useEvent, useInterval } from '../common/hooks';
import { ParamList } from '../common/ParamList';
import { deleteScript, loadSource, runScript, saveSource, stopScript, validName } from '../common/scripts';
import { Editor } from './Editor';
import { Help } from './Help';

const TEMPLATE = `// New effect. frame(t, dt) runs every DMX frame; t and dt are in seconds.
include('setup');   // the lights (heads, TYPES, ready()): setup.js

var phase = 0;

function frame(t, dt) {
  var speed = param('speed', 0.2, 0, 1);   // a live slider, 0..1
  phase += speed * dt;
  ready();
  heads.forEach(function (h, i) {
    h.dim = 255;
    h.pan = 127.5 + 60 * Math.sin(2 * Math.PI * (phase + i / heads.length));
  });
}
`;

// "line 12: ..." -> {file: script, line: 12}; "setup.js line 7: ..." -> {file: 'setup', line: 7}
function errorAt(failed) {
  const m = /^(?:([\w-]+)\.js )?line (\d+):/.exec(failed.error || '');
  return m ? { file: m[1] || failed.name, line: +m[2] } : null;
}

export function App() {
  const [st, setSt] = useState(null);
  const [sel, setSel] = useState(() => new URLSearchParams(location.search).get('name') || '');
  const [src, setSrc] = useState('');
  const [saved, setSaved] = useState('');        // what the bridge has for `sel`
  const [isNew, setIsNew] = useState(false);
  const [newName, setNewName] = useState('');
  const [msg, setMsg] = useState({ text: '', bad: false });
  const [log, setLog] = useState('');
  const [jump, setJump] = useState(null);
  const [delArmed, tapDel, disarmDel] = useConfirm(4000);
  const drafts = useRef({});                      // unsaved edits per script, kept while switching
  const pendingJump = useRef(null);               // {file, line} to select once `file` has loaded
  const logEnd = useRef(-1);
  const logBox = useRef();

  const say = (text, bad = false) => setMsg({ text, bad });
  const dirty = isNew || src !== saved;

  const refresh = async () => {
    try {
      const s = await getJson('/api/scripts');
      setSt(s);
      if (s.log_end !== logEnd.current) {
        const r = await getJson(`/api/scripts/log?since=${Math.max(0, logEnd.current)}`);
        setLog(l => (logEnd.current < 0 || r.end < logEnd.current ? r.text : (l + r.text).slice(-8000)));
        logEnd.current = r.end;
      }
    } catch (e) {}
  };
  useInterval(refresh, 1000);
  useEffect(() => { if (logBox.current) logBox.current.scrollTop = 1e9; }, [log]);

  // Pick the first script once the list is known.
  useEffect(() => {
    if (st && !sel && !isNew && st.scripts.length) setSel([...st.scripts].sort((a, b) => (!!a.lib - !!b.lib) || byName(a, b))[0].name);
  }, [st, sel, isNew]);

  useEffect(() => {
    if (!sel || isNew) return;
    disarmDel();
    let gone = false;
    loadSource(sel).then(text => {
      if (gone) return;
      setSaved(text);
      setSrc(drafts.current[sel] ?? text);
      const pj = pendingJump.current;
      if (pj && pj.file === sel) { pendingJump.current = null; setJump({ line: pj.line, n: Date.now() }); }
    }).catch(() => { if (!gone) { setSaved(''); setSrc(''); say(`Could not load ${sel}`, true); } });
    history.replaceState(null, '', `?name=${encodeURIComponent(sel)}`);
    return () => { gone = true; };
  }, [sel, isNew]);

  useEvent(window, 'beforeunload', e => {
    if (dirty || Object.keys(drafts.current).length) { e.preventDefault(); e.returnValue = ''; }
  });

  const edit = text => {
    setSrc(text);
    if (!isNew) {
      if (text === saved) delete drafts.current[sel]; else drafts.current[sel] = text;
    }
  };

  const pick = name => {
    setIsNew(false);
    setSel(name);
    say('');
  };

  const startNew = () => {
    setIsNew(true);
    setNewName('');
    setSrc(TEMPLATE);
    say('');
  };

  // Save; returns the name saved under, or '' on failure.
  const save = async () => {
    const name = isNew ? newName.trim() : sel;
    if (!validName(name)) { say('Name: 1-24 letters, digits, - or _', true); return ''; }
    if (isNew && st && st.scripts.some(s => s.name === name)) { say(`${name} already exists`, true); return ''; }
    try {
      const r = await saveSource(name, src);
      if (!r.ok) { say(r.error || 'Save failed', true); return ''; }
      setSt(r);
      delete drafts.current[name];
      setSaved(src);
      if (isNew) { setIsNew(false); setSel(name); }
      const restarted = r.running === name;
      const failed = r.failed && r.failed.name === name;
      say(failed ? 'Saved – the script failed to restart' : restarted ? 'Saved – restarted' : 'Saved', failed);
      return name;
    } catch (e) { say('Save failed (connection)', true); return ''; }
  };

  const run = async () => {
    const name = dirty ? await save() : sel;
    if (!name) return;
    try {
      const r = await runScript(name);
      setSt(r);
      if (r.failed && r.failed.name === name) say('Failed – see the error', true);
      else say(r.ok ? `Running ${name}` : r.error || 'Run failed', !r.ok);
    } catch (e) { say('Run failed (connection)', true); }
  };

  const stop = async () => {
    try { setSt(await stopScript()); say('Stopped'); } catch (e) {}
  };

  const del = async () => {
    if (!tapDel()) return;
    try {
      const r = await deleteScript(sel);
      if (!r.ok) return say(r.error || 'Delete failed', true);
      delete drafts.current[sel];
      setSt(r);
      setSel('');
      setSrc('');
      setSaved('');
      say(`Deleted ${sel}`);
    } catch (e) { say('Delete failed (connection)', true); }
  };

  const download = () => {
    const a = document.createElement('a');
    a.href = URL.createObjectURL(new Blob([src], { type: 'text/javascript' }));
    a.download = `${isNew ? newName || 'script' : sel}.js`;
    a.click();
    setTimeout(() => URL.revokeObjectURL(a.href), 1000);
  };

  const upload = async e => {
    const f = e.currentTarget.files[0];
    e.currentTarget.value = '';
    if (!f) return;
    const name = f.name.replace(/\.js$/, '');
    if (!validName(name)) return say('File name: 1-24 letters, digits, - or _, ending in .js', true);
    try {
      const r = await saveSource(name, await f.text());
      if (!r.ok) return say(r.error || 'Upload failed', true);
      setSt(r);
      delete drafts.current[name];
      pick(name);
      say(`Uploaded ${name}`);
    } catch (e2) { say('Upload failed (connection)', true); }
  };

  const running = st && st.running;
  const selRunning = !isNew && running === sel;
  // An error shows with its own script, and with the shared file it points into.
  const failedAt = st && st.failed ? errorAt(st.failed) : null;
  const failed = st && st.failed && !isNew && (st.failed.name === sel || (failedAt && failedAt.file === sel))
    ? st.failed : null;
  const all = st ? [...st.scripts].sort(byName) : [];
  const scripts = all.filter(s => !s.lib), libs = all.filter(s => s.lib);
  const selLib = !isNew && libs.some(s => s.name === sel);
  const gotoError = () => {
    if (!failedAt) return;
    if (failedAt.file === sel) setJump({ line: failedAt.line, n: Date.now() });
    else { pendingJump.current = failedAt; pick(failedAt.file); }
  };
  const item = s => (
    <button key={s.name} type="button"
            class={'item' + (s.name === sel && !isNew ? ' sel' : '')}
            onClick={() => pick(s.name)}>
      <span class={'dot' + (s.name === running ? ' on' : '')}
            title={s.name === running ? 'running' : ''} />
      <span class="nm">{s.name}</span>
      {drafts.current[s.name] !== undefined && <span class="mod" title="unsaved changes">●</span>}
    </button>
  );

  return (
    <main class="wide">
      <h1>Effect scripts
        <span class="sp" />
        <a href="/console"><button type="button" class="sec">Console</button></a>
        <a href="/"><button type="button" class="sec">Home</button></a>
        <a href="/settings"><button type="button" class="sec">Settings</button></a></h1>

      <div class="layout">
        <section class="card list">
          <h2>Scripts</h2>
          {scripts.map(item)}
          {isNew && <div class="item sel"><span class="dot" /><span class="nm">new script</span></div>}
          {libs.length > 0 && <h2 class="sub" title="files effects load with include()">Shared</h2>}
          {libs.map(item)}
          <div class="actions" style={{ marginTop: '8px' }}>
            <button type="button" class="sec" onClick={startNew}>+ New</button>
            <label class="sec upl" title="upload a .js file from this computer">Upload
              <input type="file" accept=".js,text/javascript" onChange={upload} hidden /></label>
          </div>
          {st && st.fs_total > 0 && (
            <div class="hint">{Math.round(st.fs_used / 1024)} of {Math.round(st.fs_total / 1024)} KB used</div>
          )}
        </section>

        <section class="card main">
          <div class="bar">
            {isNew ? (
              <input class="name" placeholder="name, e.g. strobe-chase" value={newName} maxLength={24}
                     onInput={e => setNewName(e.currentTarget.value)} />
            ) : (
              <b class="title">{sel || '–'}{dirty && <span class="mod"> ●</span>}</b>
            )}
            <span class="sp" />
            <button type="button" class="sec" disabled={!dirty || (!sel && !isNew)} onClick={save}
                    title="Ctrl+S">Save</button>
            {!selLib && (
              <button type="button" disabled={!sel && !isNew} onClick={run}
                      title={dirty ? 'save, then run' : 'run'}>{selRunning ? 'Restart' : 'Run'}</button>
            )}
            <button type="button" class="sec" disabled={!running} onClick={stop}
                    title={running ? `stop ${running}` : 'nothing running'}>Stop</button>
            <button type="button" class="sec" disabled={!sel && !isNew} onClick={download}>Download</button>
            {!isNew && sel && (
              <button type="button" class="danger" onClick={del}>{delArmed ? 'Tap again' : 'Delete'}</button>
            )}
          </div>
          {failed && (
            <button type="button" class="err" title="go to the line" onClick={gotoError}>
              {failed.name !== sel && `${failed.name}: `}{failed.error}
            </button>
          )}
          <Editor value={src} onInput={edit} onSave={save} jump={jump} />
          <div class="actions">
            <span id="msg" style={{ color: msg.bad ? 'var(--bad)' : 'var(--ok)' }}>{msg.text}</span>
            <span class="sp" />
            {running && <span class="hint">running: <b>{running}</b>
              {st.heap_limit ? ` · ${Math.round(st.heap_used / 1024)} KB` : ''}</span>}
          </div>
        </section>
      </div>

      {running && st.params.length > 0 && (
        <section class="card">
          <h2>Parameters – {running}</h2>
          <ParamList params={st.params} running={running} />
        </section>
      )}

      <section class="card">
        <div class="bar">
          <h2 style={{ margin: 0 }}>Log</h2>
          <span class="sp" />
          <button type="button" class="sec small" onClick={() => setLog('')}>Clear</button>
        </div>
        <pre ref={logBox} class="log">{log || 'print() output and errors show up here.'}</pre>
      </section>

      <Help />
    </main>
  );
}

const byName = (a, b) => a.name.localeCompare(b.name);
