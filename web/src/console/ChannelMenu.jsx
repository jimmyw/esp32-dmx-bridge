import { useLayoutEffect, useRef, useState } from 'preact/hooks';

/*
 * Popup opened from a strip's name bar: name the channel or hide it. Unnamed channels are
 * hidden as soon as any channel has a name, so for those the hide button is disabled and
 * naming is the way to show them.
 */
export function ChannelMenu({ ch, names, hidden, nameMax, onApply, onClose, onTab }) {
  const [value, setValue] = useState(names[ch] || '');
  const ref = useRef();
  const input = useRef();

  // Place the menu under the strip's name bar, kept on screen.
  useLayoutEffect(() => {
    const nm = document.querySelector(`.strip[data-ch="${ch}"] .nm`);
    const m = ref.current;
    if (nm) {
      const r = nm.getBoundingClientRect();
      m.style.left = Math.max(8, Math.min(innerWidth - m.offsetWidth - 8, r.left + r.width / 2 - m.offsetWidth / 2)) + 'px';
      m.style.top = Math.max(8, Math.min(innerHeight - m.offsetHeight - 8, r.bottom + 6)) + 'px';
    }
    if (matchMedia('(pointer:fine)').matches) {   // no surprise keyboard on phones
      input.current.focus();
      input.current.select();
    }
  }, [ch]);

  const v = value.trim();
  const named = Object.keys(names);
  const onlyThisNamed = named.length === 1 && names[ch];
  const unnamedHidden = named.length > 0 && !v && !onlyThisNamed;
  const isHiddenFlag = hidden.has(ch);

  const key = e => {
    if (e.key === 'Enter') { e.preventDefault(); onApply(ch, v); }
    else if (e.key === 'Escape') onClose();
    else if (e.key === 'Tab') { e.preventDefault(); onTab(ch, v, e.shiftKey ? -1 : 1); }
  };

  return (
    <>
      <div id="chback" onClick={onClose} />
      <div id="chmenu" ref={ref} role="dialog" aria-label="channel settings">
        <span class="t">Channel {ch + 1}</span>
        <input ref={input} value={value} maxLength={nameMax} placeholder={`Name for ch ${ch + 1}`}
               onInput={e => setValue(e.currentTarget.value)} onKeyDown={key} />
        <button disabled={unnamedHidden} onClick={() => onApply(ch, v, !isHiddenFlag)}
                title={unnamedHidden ? 'unnamed channels are hidden – give it a name to show it' : ''}>
          {unnamedHidden ? 'Hidden (no name)' : isHiddenFlag ? 'Show channel again' : 'Hide channel'}
        </button>
        <div class="row">
          <button onClick={() => onApply(ch, v)}>Save</button>
          <button onClick={onClose}>Cancel</button>
        </div>
      </div>
    </>
  );
}
