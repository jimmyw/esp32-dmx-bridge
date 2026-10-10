import { useEffect, useRef } from 'preact/hooks';

// Plain textarea with a line-number gutter: Tab indents, Ctrl/Cmd+S saves.
// `jump` = {line, n}: select that line (n changes on every request).
export function Editor({ value, onInput, onSave, jump }) {
  const ta = useRef();
  const gutter = useRef();
  const lines = value.split('\n').length;

  useEffect(() => {
    if (!jump || !ta.current) return;
    const t = ta.current, all = t.value.split('\n');
    const line = Math.max(1, Math.min(all.length, jump.line));
    const start = all.slice(0, line - 1).reduce((n, l) => n + l.length + 1, 0);
    t.focus();
    t.setSelectionRange(start, start + all[line - 1].length);
    const lh = parseFloat(getComputedStyle(t).lineHeight) || 18;
    t.scrollTop = Math.max(0, (line - 4) * lh);
  }, [jump]);

  const keyDown = e => {
    if ((e.ctrlKey || e.metaKey) && e.key === 's') {
      e.preventDefault();
      onSave();
    } else if (e.key === 'Tab' && !e.ctrlKey && !e.altKey) {
      e.preventDefault();
      // execCommand keeps the browser's undo history working
      document.execCommand('insertText', false, '  ');
    }
  };

  return (
    <div class="editor">
      <pre ref={gutter} class="gutter" aria-hidden="true">
        {Array.from({ length: lines }, (_, i) => i + 1).join('\n')}
      </pre>
      <textarea ref={ta} value={value} spellcheck={false} wrap="off" autocapitalize="off" autocomplete="off"
                onInput={e => onInput(e.currentTarget.value)} onKeyDown={keyDown}
                onScroll={e => { gutter.current.scrollTop = e.currentTarget.scrollTop; }} />
    </div>
  );
}
