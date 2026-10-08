const N = 512;
const manual = new Uint8Array(N), output = new Uint8Array(N);
let master = 255, bankSize = 16, bankCh = 0, ws = null, pct = false;
const hidden = new Set();           // 0-based channels hidden on the console
let showHidden = false;
let chans = [];                     // channels that get a strip, in order
let page = 0;                       // index into chans of the first strip
const touched = new Map();          // ch -> timestamp of last local change (ignore echoes briefly)
const pending = new Map();          // ch -> value to send
const MASTER = 0xFFFF;
const strips = [];                  // visible strip objects

try {
  pct = localStorage.getItem('pct') === '1'; bankCh = +localStorage.getItem('bank') || 0;
  showHidden = localStorage.getItem('showHidden') === '1';
} catch (e) {}

const fmt = v => pct ? Math.round(v / 2.55) + '%' : String(v);

function makeStrip(parent, isMaster) {
  const el = document.createElement('div');
  if (!isMaster) el.className = 'strip';
  el.innerHTML = `<div class="nm"></div><div class="val"></div>
    <div class="track"><div class="slot"></div><div class="fill"></div><div class="meter"></div><div class="cap"></div></div>
    <div class="ch"></div>
    <div class="btns"><button class="fl">${isMaster ? 'DBO' : 'Flash'}</button><button class="zero">${isMaster ? 'Full' : '0'}</button></div>`;
  if (isMaster) { parent.append(...el.childNodes); } else { parent.appendChild(el); }
  const root = isMaster ? parent : el;
  const s = {
    root, ch: -1, value: 0, flash: false, dragging: false,
    val: root.querySelector('.val'), track: root.querySelector('.track'), cap: root.querySelector('.cap'),
    fill: root.querySelector('.fill'), meter: root.querySelector('.meter'), label: root.querySelector('.ch'),
    fl: root.querySelector('.fl'), zero: root.querySelector('.zero'), isMaster,
    nm: root.querySelector('.nm'),
  };
  bindFader(s);
  if (!isMaster) s.nm.addEventListener('click', () => openChanMenu(s));
  return s;
}

function geom(s) {
  const r = s.track.getBoundingClientRect();
  return { top: r.top + 10, h: Math.max(1, r.height - 20) };
}

function setLocal(s, v) {
  v = Math.max(0, Math.min(255, Math.round(v)));
  const ch = s.isMaster ? MASTER : s.ch;
  if (s.isMaster) master = v; else manual[s.ch] = v;
  touched.set(ch, performance.now());
  pending.set(ch, v);
  render(s);
  scheduleSend();
}

function bindFader(s) {
  let offset = 0;
  const fromY = y => { const g = geom(s); return (1 - (y - offset - g.top) / g.h) * 255; };
  s.track.addEventListener('pointerdown', e => {
    s.track.setPointerCapture(e.pointerId);
    s.dragging = true;
    const g = geom(s), capY = g.top + (1 - cur(s) / 255) * g.h;
    offset = Math.abs(e.clientY - capY) < 16 ? e.clientY - capY : 0;   // grab cap = relative, else jump
    setLocal(s, fromY(e.clientY));
  });
  s.track.addEventListener('pointermove', e => { if (s.dragging) setLocal(s, fromY(e.clientY)); });
  const end = () => { s.dragging = false; };
  s.track.addEventListener('pointerup', end);
  s.track.addEventListener('pointercancel', end);
  s.track.addEventListener('wheel', e => {
    e.preventDefault();
    setLocal(s, cur(s) + (e.deltaY < 0 ? 1 : -1) * (e.shiftKey ? 10 : 1));
  }, { passive: false });

  // Flash (strip) / DBO (master): momentary
  const down = e => {
    e.preventDefault(); s.fl.classList.add('down');
    if (s.isMaster) { s.saved = master; setLocal(s, 0); }
    else { s.saved = manual[s.ch]; setLocal(s, 255); }
  };
  const up = () => {
    if (!s.fl.classList.contains('down')) return;
    s.fl.classList.remove('down'); setLocal(s, s.saved);
  };
  s.fl.addEventListener('pointerdown', down);
  s.fl.addEventListener('pointerup', up);
  s.fl.addEventListener('pointerleave', up);
  s.fl.addEventListener('pointercancel', up);
  s.zero.addEventListener('click', () => setLocal(s, s.isMaster ? 255 : 0));
}

const cur = s => s.isMaster ? master : manual[s.ch];

function render(s) {
  if (s.ch < 0) return;
  const v = cur(s);
  s.val.textContent = fmt(v);
  const g = s.track.clientHeight - 20;
  s.cap.style.top = (10 + (1 - v / 255) * g) + 'px';
  s.fill.style.height = (v / 255 * g) + 'px';
  if (!s.isMaster) s.meter.style.height = (output[s.ch] / 255 * g) + 'px';
}

const masterStrip = makeStrip(document.getElementById('masterStrip'), true);
masterStrip.label.textContent = 'Master';
masterStrip.nm.textContent = 'MASTER';
masterStrip.meter.style.display = 'none';

function layout() {
  const w = window.innerWidth;
  const size = w >= 1100 ? 16 : w >= 700 ? 12 : 8;
  if (size !== bankSize || strips.length === 0) {
    bankSize = size;
    const box = document.getElementById('strips');
    box.innerHTML = ''; strips.length = 0;
    box.style.gridTemplateColumns = `repeat(${size}, minmax(0,1fr))`;
    for (let i = 0; i < size; i++) strips.push(makeStrip(box, false));
  }
  chans = [];
  hiddenCount = 0;
  for (let c = 0; c < N; c++) {
    if (!isHidden(c)) chans.push(c);
    else { hiddenCount++; if (showHidden) chans.push(c); }
  }
  // Keep the bank that holds bankCh (or the next visible channel after it).
  let idx = chans.findIndex(c => c >= bankCh);
  if (idx < 0) idx = chans.length - 1;
  page = Math.max(0, Math.floor(idx / bankSize) * bankSize);
  strips.forEach((s, i) => {
    s.ch = page + i < chans.length ? chans[page + i] : -1;
    s.root.classList.toggle('blank', s.ch < 0);
    s.root.classList.toggle('hid', s.ch >= 0 && isHidden(s.ch));
    s.label.textContent = s.ch < 0 ? '' : s.ch + 1;
    renderName(s);
  });
  const last = Math.min(chans.length, page + bankSize) - 1;
  document.getElementById('bank').textContent =
    chans.length ? `Ch ${chans[page] + 1}–${chans[last] + 1}` : 'All hidden';
  if (chans.length) bankCh = chans[page];
  try { localStorage.setItem('bank', bankCh); } catch (e) {}
  renderHidBtn();
  renderAll();
}

function renderAll() { strips.forEach(render); render(masterStrip); }

function go(delta) {
  if (!chans.length) return;
  const p = Math.max(0, Math.min(Math.floor((chans.length - 1) / bankSize) * bankSize, page + delta * bankSize));
  bankCh = chans[p]; layout();
}
document.getElementById('prev').onclick = () => go(-1);
document.getElementById('next').onclick = () => go(1);
document.getElementById('jump').addEventListener('change', e => {
  const c = Math.max(1, Math.min(N, +e.target.value || 1)) - 1;
  if (isHidden(c) && !showHidden) { showHidden = true; saveShowHidden(); }   // jumping to a hidden one
  bankCh = c; e.target.value = ''; layout();
});
document.getElementById('units').onclick = e => {
  pct = !pct; e.target.textContent = pct ? '%' : 'DMX';
  try { localStorage.setItem('pct', pct ? '1' : '0'); } catch (err) {}
  renderAll();
};
document.getElementById('units').textContent = pct ? '%' : 'DMX';

let clearArmed = 0;
document.getElementById('clear').onclick = e => {
  if (performance.now() - clearArmed > 2500) { clearArmed = performance.now(); e.target.textContent = 'Sure?';
    setTimeout(() => e.target.textContent = 'Clear', 2500); return; }
  clearArmed = 0; e.target.textContent = 'Clear';
  manual.fill(0); touched.clear(); pending.clear();
  for (let i = 0; i < N; i++) touched.set(i, performance.now());
  if (ws && ws.readyState === 1) ws.send('clear');
  renderAll();
};

window.addEventListener('keydown', e => {
  if (e.target.tagName === 'INPUT') return;
  if (e.key >= '1' && e.key <= '9' && !e.ctrlKey && !e.metaKey && !e.altKey) {
    const sc = scenes[+e.key - 1]; if (sc && !editMode) recall(sc.id);
  }
  if (e.key === 'ArrowLeft' || e.key === 'PageUp') go(-1);
  if (e.key === 'ArrowRight' || e.key === 'PageDown') go(1);
});

// ---- channel names / hidden channels (stored on the bridge, shared by all clients) ----
const names = {};   // 0-based channel -> name
let nameMax = 24, hiddenCount = 0;
// Unnamed channels are hidden once any channel has a name; named ones only when hidden explicitly.
const anyNamed = () => { for (const k in names) return true; return false; };
const isHidden = c => anyNamed() && (!names[c] || hidden.has(c));

function renderName(s) {
  if (s.ch < 0) { s.nm.textContent = ''; s.root.title = ''; return; }
  const n = names[s.ch] || '';
  s.nm.textContent = n || 'name';
  s.nm.classList.toggle('empty', !n);
  s.root.title = (n ? `Ch ${s.ch + 1}: ${n}` : `Ch ${s.ch + 1}`) + (isHidden(s.ch) ? (names[s.ch] ? ' (hidden)' : ' (hidden: no name)') : '');
}

function saveShowHidden() { try { localStorage.setItem('showHidden', showHidden ? '1' : '0'); } catch (e) {} }

function renderHidBtn() {
  const b = document.getElementById('hidBtn');
  b.hidden = hiddenCount === 0;
  b.textContent = `Hidden ${hiddenCount}`;
  b.title = showHidden ? 'showing hidden channels (dimmed) – tap to hide them' : 'show hidden channels';
  b.classList.toggle('on', showHidden);
}
document.getElementById('hidBtn').onclick = () => { showHidden = !showHidden; saveShowHidden(); layout(); };

async function fetchNames() {
  try {
    const r = await (await fetch('/api/names')).json();
    nameMax = r.max_len || nameMax;
    for (const k in names) delete names[k];
    for (const [ch, n] of Object.entries(r.names)) names[+ch - 1] = n;
    hidden.clear();
    for (const ch of r.hidden || []) hidden.add(ch - 1);
    layout();
    if (!hiddenCount && showHidden) { showHidden = false; saveShowHidden(); }
  } catch (e) {}
}

async function postNames(body) {
  try {
    await fetch('/api/names', {method: 'POST', headers: {'Content-Type': 'application/json'},
                               body: JSON.stringify(body)});
  } catch (e) {}
}

// Channel menu: opened by tapping a strip's name bar.
let menuCh = -1;
function openChanMenu(s) {
  if (s.ch < 0) return;
  const ch = menuCh = s.ch;
  const m = document.getElementById('chmenu'), inp = document.getElementById('cmName');
  document.getElementById('cmTitle').textContent = `Channel ${ch + 1}`;
  inp.value = names[ch] || ''; inp.maxLength = nameMax; inp.placeholder = `Name for ch ${ch + 1}`;
  renderHideBtn();
  m.hidden = false; document.getElementById('chback').hidden = false;
  const r = s.nm.getBoundingClientRect(), w = m.offsetWidth, h = m.offsetHeight;
  m.style.left = Math.max(8, Math.min(innerWidth - w - 8, r.left + r.width / 2 - w / 2)) + 'px';
  m.style.top = Math.max(8, Math.min(innerHeight - h - 8, r.bottom + 6)) + 'px';
  if (matchMedia('(pointer:fine)').matches) { inp.focus(); inp.select(); }   // no surprise keyboard on phones
}
// Unnamed channels can't be shown on their own: naming one shows it.
function renderHideBtn() {
  const ch = menuCh, b = document.getElementById('cmHide');
  const v = document.getElementById('cmName').value.trim();
  const unnamedHidden = anyNamed() && !v && !(Object.keys(names).length === 1 && names[ch]);
  b.disabled = unnamedHidden;
  b.textContent = unnamedHidden ? 'Hidden (no name)' : hidden.has(ch) ? 'Show channel again' : 'Hide channel';
  b.title = b.disabled ? 'unnamed channels are hidden – give it a name to show it' : '';
}
document.getElementById('cmName').addEventListener('input', renderHideBtn);
function closeChanMenu() {
  menuCh = -1;
  document.getElementById('chmenu').hidden = true; document.getElementById('chback').hidden = true;
}
// Apply the menu; hide: true/false to change the hidden flag, undefined to leave it.
function applyChanMenu(hide) {
  const ch = menuCh;
  if (ch < 0) return;
  const v = document.getElementById('cmName').value.trim(), body = {};
  if (v !== (names[ch] || '')) {
    // Naming a channel shows it, even if it was hidden explicitly while unnamed.
    if (v && !names[ch] && hidden.has(ch) && hide === undefined) hide = false;
    body[ch + 1] = v; if (v) names[ch] = v; else delete names[ch];
  }
  if (hide !== undefined && hide !== hidden.has(ch)) {
    body.hidden = {[ch + 1]: hide};
    if (hide) hidden.add(ch); else hidden.delete(ch);
  }
  closeChanMenu();
  if (Object.keys(body).length) { postNames(body); layout(); }
}
document.getElementById('cmSave').onclick = () => applyChanMenu();
document.getElementById('cmHide').onclick = () => applyChanMenu(!hidden.has(menuCh));
document.getElementById('cmCancel').onclick = closeChanMenu;
document.getElementById('chback').onclick = closeChanMenu;
document.getElementById('cmName').addEventListener('keydown', e => {
  if (e.key === 'Enter') { e.preventDefault(); applyChanMenu(); }
  else if (e.key === 'Escape') closeChanMenu();
  else if (e.key === 'Tab') {               // Tab = save and edit the next strip's name
    e.preventDefault();
    const i = strips.findIndex(x => x.ch === menuCh) + (e.shiftKey ? -1 : 1);
    applyChanMenu();
    if (strips[i] && strips[i].ch >= 0) setTimeout(() => openChanMenu(strips[i]), 0);
  }
});

// ---- scenes (stored on the bridge) ----
let scenes = [], sceneCount = 64, activeScene = 0, fading = false, editMode = false, editId = 0, delArmed = false;
let barDelArmed = 0, barTimer = 0;
const $ = id => document.getElementById(id);
try { $('fade').value = localStorage.getItem('fade') || '0'; } catch (e) {}
$('fade').addEventListener('change', () => { try { localStorage.setItem('fade', $('fade').value); } catch (e) {} });

async function fetchScenes() {
  try {
    const r = await (await fetch('/api/scenes')).json();
    scenes = r.scenes; sceneCount = r.count; activeScene = r.active; fading = r.fading;
    renderScenes();
  } catch (e) {}
}

function renderSceneActions() {
  const sc = !editMode && scenes.find(x => x.id === activeScene);
  $('updBtn').hidden = $('delBtn').hidden = !sc;
  if (!sc) { barDelArmed = 0; return; }
  if (barDelArmed !== sc.id) barDelArmed = 0;
  $('updBtn').title = `overwrite "${sc.name}" with the current faders`;
  $('delBtn').title = `delete "${sc.name}"`;
  $('delBtn').textContent = barDelArmed ? 'Tap again to delete' : 'Delete';
}
function flashBtn(b, text) {
  const old = b.dataset.label || (b.dataset.label = b.textContent);
  b.textContent = text; clearTimeout(barTimer);
  barTimer = setTimeout(() => { b.textContent = old; }, 1500);
}

function renderScenes() {
  renderSceneActions();
  const box = $('scenes');
  box.innerHTML = '';
  const add = (label, cls, fn, title) => {
    const b = document.createElement('button');
    b.textContent = label; if (cls) b.className = cls; if (title) b.title = title;
    b.onclick = fn; box.appendChild(b); return b;
  };
  if (editMode) {
    const used = new Map(scenes.map(sc => [sc.id, sc.name]));
    for (let id = 1; id <= sceneCount; id++) {
      const n = used.get(id);
      add(n ? `${id}. ${n}` : `${id}. –`, (n ? '' : 'empty') + (id === editId ? ' active' : ''), () => openEditor(id));
    }
    return;
  }
  scenes.forEach((sc, i) => {
    const cls = (sc.id === activeScene ? 'active' : '') + (sc.id === activeScene && fading ? ' fading' : '');
    const b = add(sc.name, cls, () => recall(sc.id),
                  `Scene ${sc.id}` + (i < 9 ? ` (key ${i + 1})` : '') + ' – right-click / long-press to edit');
    b.oncontextmenu = e => {                      // right-click or long-press: edit this scene
      e.preventDefault(); editMode = true; $('editBtn').classList.add('on'); openEditor(sc.id);
    };
  });
  add('+ Save', 'new', () => {
    const used = new Set(scenes.map(sc => sc.id));
    let id = 1; while (used.has(id) && id <= sceneCount) id++;
    if (id > sceneCount) { editMode = true; $('editBtn').classList.add('on'); renderScenes(); return; }
    openEditor(id);
  }, 'save the current console faders as a new scene');
  if (!scenes.length) {
    const t = document.createElement('span'); t.className = 'empty';
    t.textContent = 'Set faders, then + Save'; box.appendChild(t);
  }
}

async function sceneApi(body) {
  try {
    const r = await (await fetch('/api/scenes', {method: 'POST', headers: {'Content-Type': 'application/json'},
                                                 body: JSON.stringify(body)})).json();
    if (!r.ok) $('msg').textContent = r.error || 'failed';
    return r.ok;
  } catch (e) { $('msg').textContent = 'connection failed'; return false; }
}

function recall(id) {
  const fade = Math.max(0, Math.min(60, parseFloat($('fade').value) || 0));
  activeScene = id; fading = fade > 0; renderScenes();
  sceneApi({action: 'recall', id, fade_ms: Math.round(fade * 1000)});
}

function openEditor(id) {
  editId = id; delArmed = false;
  const sc = scenes.find(x => x.id === id);
  $('editor').hidden = false;
  $('edTitle').textContent = sc ? `Scene ${id}` : `New scene ${id}`;
  $('edName').value = sc ? sc.name : '';
  $('edName').placeholder = `Scene ${id}`;
  $('edRename').hidden = $('edDelete').hidden = !sc;
  $('edSave').textContent = sc ? 'Overwrite with faders' : 'Save faders';
  $('edDelete').textContent = 'Delete';
  $('msg').textContent = '';
  $('edName').focus();
  renderScenes();
}
function closeEditor() { $('editor').hidden = true; editId = 0; renderScenes(); }

$('edSave').onclick = async () => {
  if (await sceneApi({action: 'save', id: editId, name: $('edName').value.trim()})) closeEditor();
};
$('edRename').onclick = async () => {
  if (await sceneApi({action: 'rename', id: editId, name: $('edName').value.trim()})) closeEditor();
};
$('edDelete').onclick = async () => {
  if (!delArmed) { delArmed = true; $('edDelete').textContent = 'Tap again to delete'; return; }
  if (await sceneApi({action: 'delete', id: editId})) closeEditor();
};
$('edCancel').onclick = closeEditor;
$('updBtn').onclick = async () => {
  const id = activeScene;
  if (await sceneApi({action: 'save', id, name: ''})) flashBtn($('updBtn'), 'Saved ✓');
  else flashBtn($('updBtn'), $('msg').textContent || 'failed');
};
$('delBtn').onclick = async () => {
  const id = activeScene;
  if (barDelArmed !== id) { barDelArmed = id; renderSceneActions(); return; }
  barDelArmed = 0;
  if (await sceneApi({action: 'delete', id})) { activeScene = 0; fetchScenes(); }
  else { renderSceneActions(); flashBtn($('delBtn'), $('msg').textContent || 'failed'); }
};
$('edName').addEventListener('keydown', e => {
  if (e.key === 'Enter') $('edSave').click();
  if (e.key === 'Escape') closeEditor();
});
$('editBtn').onclick = () => {
  editMode = !editMode; $('editBtn').classList.toggle('on', editMode);
  if (!editMode) closeEditor(); else renderScenes();
};

// ---- fullscreen ----
{
  const el = document.documentElement;
  const req = el.requestFullscreen || el.webkitRequestFullscreen;
  const cur = () => document.fullscreenElement || document.webkitFullscreenElement;
  const exit = () => (document.exitFullscreen || document.webkitExitFullscreen).call(document);
  const fsBtn = $('fsBtn');
  const sync = () => {
    const on = !!cur();
    fsBtn.textContent = on ? '⛶ Exit' : '⛶ Full';
    fsBtn.classList.toggle('on', on);
  };
  const toggle = () => {
    try {
      const p = cur() ? exit() : req.call(el, {navigationUI: 'hide'});
      if (p && p.catch) p.catch(() => {});
    } catch (e) {}
  };
  if (req) {                                    // iPhone Safari has no fullscreen API: keep it hidden
    fsBtn.hidden = false;
    fsBtn.onclick = toggle;
    document.addEventListener('fullscreenchange', sync);
    document.addEventListener('webkitfullscreenchange', sync);
    document.addEventListener('keydown', e => {
      if ((e.key === 'f' || e.key === 'F') && !e.ctrlKey && !e.metaKey && !e.altKey &&
          !/^(INPUT|TEXTAREA)$/.test(e.target.tagName)) toggle();
    });
  }
}

// ---- network ----
let sendTimer = null, lastSend = 0;
function scheduleSend() {
  if (sendTimer) return;
  const wait = Math.max(0, 25 - (performance.now() - lastSend));
  sendTimer = setTimeout(flush, wait);
}
function flush() {
  sendTimer = null;
  if (!ws || ws.readyState !== 1 || pending.size === 0) return;
  const buf = new Uint8Array(pending.size * 3);
  let i = 0;
  for (const [ch, v] of pending) { buf[i++] = ch >> 8; buf[i++] = ch & 255; buf[i++] = v; }
  pending.clear();
  ws.send(buf);
  lastSend = performance.now();
}

function connect() {
  ws = new WebSocket(`ws://${location.host}/ws`);
  ws.binaryType = 'arraybuffer';
  ws.onopen = () => { document.getElementById('conn').className = 'dot on'; flush(); fetchNames(); fetchScenes(); };
  ws.onclose = () => { document.getElementById('conn').className = 'dot'; setTimeout(connect, 1500); };
  ws.onerror = () => ws.close();
  ws.onmessage = ev => {
    if (typeof ev.data === 'string') {
      if (ev.data === 'names') fetchNames();
      if (ev.data === 'scenes') fetchScenes();
      return;
    }
    const d = new Uint8Array(ev.data);
    if (d[0] !== 1 || d.length < 2 + 2 * N) return;
    const now = performance.now(), fresh = ch => now - (touched.get(ch) || 0) < 400;
    if (!masterStrip.dragging && !fresh(MASTER) && !masterStrip.fl.classList.contains('down')) master = d[1];
    for (let i = 0; i < N; i++) {
      if (!fresh(i)) manual[i] = d[2 + i];
      output[i] = d[2 + N + i];
    }
    renderAll();
  };
}

window.addEventListener('resize', layout);
layout();
connect();
