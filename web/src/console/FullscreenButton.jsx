import { useState } from 'preact/hooks';
import { useEvent } from '../common/hooks';

const el = document.documentElement;
const request = el.requestFullscreen || el.webkitRequestFullscreen;
const current = () => document.fullscreenElement || document.webkitFullscreenElement;
const exit = () => (document.exitFullscreen || document.webkitExitFullscreen).call(document);

function toggle() {
  try {
    const p = current() ? exit() : request.call(el, { navigationUI: 'hide' });
    if (p && p.catch) p.catch(() => {});
  } catch (e) {}
}

// Fullscreen toggle (also the F key). iPhone Safari has no fullscreen API: no button there.
export function FullscreenButton() {
  const [on, setOn] = useState(false);
  const sync = () => setOn(!!current());
  useEvent(document, 'fullscreenchange', sync);
  useEvent(document, 'webkitfullscreenchange', sync);
  useEvent(document, 'keydown', e => {
    if ((e.key === 'f' || e.key === 'F') && !e.ctrlKey && !e.metaKey && !e.altKey &&
        !/^(INPUT|TEXTAREA)$/.test(e.target.tagName)) toggle();
  });
  if (!request) return null;
  return (
    <button id="fsBtn" class={on ? 'on' : ''} title="fullscreen (F)" aria-label="toggle fullscreen"
            onClick={toggle}>{on ? '⛶ Exit' : '⛶ Full'}</button>
  );
}
