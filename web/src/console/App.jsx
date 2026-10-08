import { useCallback, useEffect, useMemo, useState } from 'preact/hooks';
import { getJson, postJson } from '../common/api';
import { useConfirm, useEvent } from '../common/hooks';
import { usePersisted } from '../common/persisted';
import { ChannelMenu } from './ChannelMenu';
import { FullscreenButton } from './FullscreenButton';
import { Scenes } from './Scenes';
import { MasterStrip, Strip } from './Strip';
import { N, clearAll, isConnected, useLive, useLiveEvents } from './live';

// As many strips as fit at least MIN_STRIP px wide (about the phone-size master), up to 16.
// Mirrors the CSS: 12px desk padding each side, 6px gaps, master 76px (60px up to 560px wide).
const MIN_STRIP = 58, GAP = 6;
function bankSizeFor(w) {
  const strips = w - 24 - (w <= 560 ? 60 : 76) - GAP;
  return Math.max(1, Math.min(16, Math.floor((strips + GAP) / (MIN_STRIP + GAP))));
}

export function App() {
  useLive();
  const [pct, setPct] = usePersisted('pct', false);
  const [bankCh, setBankCh] = usePersisted('bank', 0);
  const [showHidden, setShowHidden] = usePersisted('showHidden', false);
  const [bankSize, setBankSize] = useState(() => bankSizeFor(innerWidth));
  // Channel names (0-based ch -> name) and explicitly hidden channels, stored on the bridge.
  const [names, setNames] = useState({});
  const [hidden, setHidden] = useState(() => new Set());
  const [nameMax, setNameMax] = useState(24);
  const [namesLoaded, setNamesLoaded] = useState(false);
  const [menuCh, setMenuCh] = useState(-1);
  const [clearArmed, tapClear] = useConfirm(2500);

  useEvent(window, 'resize', () => setBankSize(bankSizeFor(innerWidth)));

  const loadNames = useCallback(async () => {
    try {
      const r = await getJson('/api/names');
      setNameMax(r.max_len || 24);
      setNames(Object.fromEntries(Object.entries(r.names).map(([ch, n]) => [ch - 1, n])));
      setHidden(new Set((r.hidden || []).map(ch => ch - 1)));
      setNamesLoaded(true);
    } catch (e) {}
  }, []);
  useLiveEvents(useCallback(ev => { if (ev === 'open' || ev === 'names') loadNames(); }, [loadNames]));

  // Unnamed channels are hidden once any channel has a name; named ones only when hidden explicitly.
  const anyNamed = Object.keys(names).length > 0;
  const isHidden = c => anyNamed && (!names[c] || hidden.has(c));

  const { chans, hiddenCount } = useMemo(() => {
    const chans = [];
    let hiddenCount = 0;
    for (let c = 0; c < N; c++) {
      if (!isHidden(c)) chans.push(c);
      else { hiddenCount++; if (showHidden) chans.push(c); }
    }
    return { chans, hiddenCount };
  }, [names, hidden, showHidden]);

  // The bank holding bankCh (or the next shown channel after it).
  let idx = chans.findIndex(c => c >= bankCh);
  if (idx < 0) idx = chans.length - 1;
  const page = Math.max(0, Math.floor(idx / bankSize) * bankSize);
  const bank = Array.from({ length: bankSize }, (_, i) => (page + i < chans.length ? chans[page + i] : -1));
  const last = Math.min(chans.length, page + bankSize) - 1;

  const go = delta => {
    if (!chans.length) return;
    const maxPage = Math.floor((chans.length - 1) / bankSize) * bankSize;
    setBankCh(chans[Math.max(0, Math.min(maxPage, page + delta * bankSize))]);
  };
  const jump = e => {
    const c = Math.max(1, Math.min(N, +e.currentTarget.value || 1)) - 1;
    if (isHidden(c)) setShowHidden(true);   // jumping to a hidden channel shows hidden ones
    setBankCh(c);
    e.currentTarget.value = '';
  };

  useEvent(window, 'keydown', e => {
    if (e.target.tagName === 'INPUT') return;
    if (e.key === 'ArrowLeft' || e.key === 'PageUp') go(-1);
    if (e.key === 'ArrowRight' || e.key === 'PageDown') go(1);
  });

  // Apply the channel menu: new name, and hide true/false (undefined = leave as is).
  const applyMenu = (ch, v, hide) => {
    const body = {};
    const nextNames = { ...names };
    const nextHidden = new Set(hidden);
    if (v !== (names[ch] || '')) {
      // Naming a channel shows it, even if it was hidden explicitly while unnamed.
      if (v && !names[ch] && hidden.has(ch) && hide === undefined) hide = false;
      body[ch + 1] = v;
      if (v) nextNames[ch] = v; else delete nextNames[ch];
    }
    if (hide !== undefined && hide !== hidden.has(ch)) {
      body.hidden = { [ch + 1]: hide };
      if (hide) nextHidden.add(ch); else nextHidden.delete(ch);
    }
    setMenuCh(-1);
    if (Object.keys(body).length) {
      setNames(nextNames);
      setHidden(nextHidden);
      postJson('/api/names', body).catch(() => {});
    }
  };
  // Tab in the menu: save, then edit the next/previous strip's name.
  const tabMenu = (ch, v, dir) => {
    applyMenu(ch, v);
    const next = bank[bank.indexOf(ch) + dir];
    if (next >= 0) setTimeout(() => setMenuCh(next), 0);
  };

  // Nothing hidden any more: switch the toggle off so the next hidden channel really hides.
  useEffect(() => { if (namesLoaded && !hiddenCount && showHidden) setShowHidden(false); },
            [namesLoaded, hiddenCount, showHidden]);

  const clear = () => { if (tapClear()) clearAll(); };

  return (
    <>
      <header>
        <h1>DMX Console</h1>
        <span class={'dot' + (isConnected() ? ' on' : '')} title="connection" />
        <button aria-label="previous bank" onClick={() => go(-1)}>◀</button>
        <span id="bank">{chans.length ? `Ch ${chans[page] + 1}–${chans[last] + 1}` : 'All hidden'}</span>
        <button aria-label="next bank" onClick={() => go(1)}>▶</button>
        <input id="jump" type="number" min="1" max="512" placeholder="Ch #" aria-label="jump to channel"
               onChange={jump} />
        <span class="sp" />
        {hiddenCount > 0 && (
          <button id="hidBtn" class={showHidden ? 'on' : ''} onClick={() => setShowHidden(!showHidden)}
                  title={showHidden ? 'showing hidden channels (dimmed) – tap to hide them' : 'show hidden channels'}>
            Hidden {hiddenCount}
          </button>
        )}
        <button title="toggle value display" onClick={() => setPct(!pct)}>{pct ? '%' : 'DMX'}</button>
        <button title="set all console faders to 0" onClick={clear}>{clearArmed ? 'Sure?' : 'Clear'}</button>
        <FullscreenButton />
        <a class="btn" href="/">Settings</a>
      </header>
      <Scenes />
      {menuCh >= 0 && (
        <ChannelMenu key={menuCh} ch={menuCh} names={names} hidden={hidden} nameMax={nameMax}
                     onApply={applyMenu} onClose={() => setMenuCh(-1)} onTab={tabMenu} />
      )}
      <div id="desk">
        <div id="strips" style={{ gridTemplateColumns: `repeat(${bankSize}, minmax(0,1fr))` }}>
          {bank.map((ch, i) => (
            <Strip key={i} ch={ch} name={names[ch] || ''} hidden={ch >= 0 && isHidden(ch)} pct={pct}
                   onMenu={setMenuCh} />
          ))}
        </div>
        <MasterStrip pct={pct} />
      </div>
    </>
  );
}
