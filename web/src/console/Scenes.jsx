import { useCallback, useEffect, useRef, useState } from 'preact/hooks';
import { getJson, postJson } from '../common/api';
import { useConfirm, useEvent, useFlash } from '../common/hooks';
import { usePersisted } from '../common/persisted';
import { useLiveEvents } from './live';

/*
 * Scene bar (recall, + Save, Update / Delete for the active scene, Edit mode) and the scene
 * editor row. Scenes are stored on the bridge: /api/scenes.
 */
// Stays mounted while the toolbar is hidden, so keys 1-9 still recall scenes.
export function Scenes({ visible }) {
  const [list, setList] = useState({ scenes: [], count: 64, active: 0, fading: false });
  const [fade, setFade] = usePersisted('fade', 0);
  const [editMode, setEditMode] = useState(false);
  const [editId, setEditId] = useState(0);
  const [barMsg, flashBarMsg] = useFlash();
  const [delArmed, tapDel, disarmDel] = useConfirm(4000);

  const load = useCallback(async () => {
    try { setList(await getJson('/api/scenes')); } catch (e) {}
  }, []);
  useLiveEvents(useCallback(ev => { if (ev === 'open' || ev === 'scenes') load(); }, [load]));

  const { scenes, count, active, fading } = list;
  const activeScene = !editMode && scenes.find(s => s.id === active);
  useEffect(disarmDel, [active]);

  const api = async body => {
    try {
      const r = await postJson('/api/scenes', body);
      return r.ok ? '' : (r.error || 'failed');
    } catch (e) { return 'connection failed'; }
  };

  const recall = id => {
    const f = Math.max(0, Math.min(60, parseFloat(fade) || 0));
    setList(l => ({ ...l, active: id, fading: f > 0 }));
    api({ action: 'recall', id, fade_ms: Math.round(f * 1000) });
  };

  const openEditor = id => { setEditId(id); };
  const enterEdit = id => { setEditMode(true); if (id) openEditor(id); };
  const closeEditor = () => setEditId(0);

  const saveNew = () => {
    const used = new Set(scenes.map(s => s.id));
    let id = 1;
    while (used.has(id) && id <= count) id++;
    if (id > count) enterEdit(0);   // all slots used: pick one to overwrite
    else openEditor(id);
  };

  const update = async () => {
    const err = await api({ action: 'save', id: active, name: '' });
    flashBarMsg(err || 'Saved ✓');
  };
  const del = async () => {
    if (!tapDel()) return;
    const err = await api({ action: 'delete', id: active });
    if (err) flashBarMsg(err); else load();
  };

  useEvent(window, 'keydown', e => {
    if (e.target.tagName === 'INPUT' || editMode) return;
    if (e.key >= '1' && e.key <= '9' && !e.ctrlKey && !e.metaKey && !e.altKey) {
      const sc = scenes[+e.key - 1];
      if (sc) recall(sc.id);
    }
  });

  if (!visible) return null;

  let buttons;
  if (editMode) {
    const used = new Map(scenes.map(s => [s.id, s.name]));
    buttons = Array.from({ length: count }, (_, i) => {
      const id = i + 1, n = used.get(id);
      return (
        <button key={id} class={(n ? '' : 'empty') + (id === editId ? ' active' : '')}
                onClick={() => openEditor(id)}>{n ? `${id}. ${n}` : `${id}. –`}</button>
      );
    });
  } else {
    buttons = scenes.map((sc, i) => (
      <button key={sc.id} class={(sc.id === active ? 'active' : '') + (sc.id === active && fading ? ' fading' : '')}
              title={`Scene ${sc.id}` + (i < 9 ? ` (key ${i + 1})` : '') + ' – right-click / long-press to edit'}
              onClick={() => recall(sc.id)}
              onContextMenu={e => { e.preventDefault(); enterEdit(sc.id); }}>{sc.name}</button>
    ));
    buttons.push(
      <button key="new" class="new" title="save the current console faders as a new scene"
              onClick={saveNew}>+ Save</button>,
    );
    if (!scenes.length) buttons.push(<span key="hint" class="empty">Set faders, then + Save</span>);
  }

  return (
    <>
      <div id="scenebar">
        <span class="lbl">Scenes</span>
        <div id="scenes">{buttons}</div>
        <label class="lbl" for="fade">Fade s</label>
        <input id="fade" type="number" min="0" max="60" step="0.1" value={fade}
               title="fade time for recall (seconds)" onChange={e => setFade(e.currentTarget.value)} />
        {activeScene && (
          <>
            <button onClick={update} title={`overwrite "${activeScene.name}" with the current faders`}>
              {barMsg || 'Update'}
            </button>
            <button class="danger" onClick={del} title={`delete "${activeScene.name}"`}>
              {delArmed ? 'Tap again to delete' : 'Delete'}
            </button>
          </>
        )}
        <button id="editBtn" class={editMode ? 'on' : ''} title="save, rename or delete scenes"
                onClick={() => { setEditMode(!editMode); closeEditor(); }}>Edit</button>
      </div>
      {editId > 0 && (
        <SceneEditor key={editId} id={editId} scene={scenes.find(s => s.id === editId)} api={api}
                     onDone={closeEditor} />
      )}
    </>
  );
}

function SceneEditor({ id, scene, api, onDone }) {
  const [name, setName] = useState(scene ? scene.name : '');
  const [msg, setMsg] = useState('');
  const [delArmed, tapDel] = useConfirm(4000);
  const input = useRef();
  useEffect(() => input.current.focus(), []);

  const run = async body => {
    const err = await api(body);
    if (err) setMsg(err); else onDone();
  };
  const save = () => run({ action: 'save', id, name: name.trim() });
  const rename = () => run({ action: 'rename', id, name: name.trim() });
  const del = () => { if (tapDel()) run({ action: 'delete', id }); };

  return (
    <div id="editor">
      <span class="t">{scene ? `Scene ${id}` : `New scene ${id}`}</span>
      <input ref={input} maxLength={24} placeholder={`Scene ${id}`} value={name}
             onInput={e => setName(e.currentTarget.value)}
             onKeyDown={e => { if (e.key === 'Enter') save(); if (e.key === 'Escape') onDone(); }} />
      <button onClick={save}>{scene ? 'Overwrite with faders' : 'Save faders'}</button>
      {scene && <button onClick={rename}>Rename</button>}
      {scene && <button class="danger" onClick={del}>{delArmed ? 'Tap again to delete' : 'Delete'}</button>}
      <button onClick={onDone}>Cancel</button>
      <span id="msg">{msg}</span>
    </div>
  );
}
