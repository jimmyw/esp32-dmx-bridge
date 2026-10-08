import { useEffect, useRef, useState } from 'preact/hooks';

// Button that needs a second tap within `ms` to act. Returns [armed, tap]; tap() returns true
// on the confirming tap.
export function useConfirm(ms = 2500) {
  const [armed, setArmed] = useState(false);
  const timer = useRef(0);
  useEffect(() => () => clearTimeout(timer.current), []);
  const tap = () => {
    clearTimeout(timer.current);
    if (armed) { setArmed(false); return true; }
    setArmed(true);
    timer.current = setTimeout(() => setArmed(false), ms);
    return false;
  };
  return [armed, tap, () => { clearTimeout(timer.current); setArmed(false); }];
}

// Text shown for a while, then cleared: [text, show(text)].
export function useFlash(ms = 1500) {
  const [text, setText] = useState('');
  const timer = useRef(0);
  useEffect(() => () => clearTimeout(timer.current), []);
  const show = t => {
    clearTimeout(timer.current);
    setText(t);
    timer.current = setTimeout(() => setText(''), ms);
  };
  return [text, show];
}

// Run `fn` every `ms` (and once immediately); always calls the latest `fn`.
export function useInterval(fn, ms) {
  const saved = useRef(fn);
  saved.current = fn;
  useEffect(() => {
    saved.current();
    const t = setInterval(() => saved.current(), ms);
    return () => clearInterval(t);
  }, [ms]);
}

// Add a window/document event listener for the component's lifetime; calls the latest handler.
export function useEvent(target, type, handler) {
  const saved = useRef(handler);
  saved.current = handler;
  useEffect(() => {
    const fn = e => saved.current(e);
    target.addEventListener(type, fn);
    return () => target.removeEventListener(type, fn);
  }, [target, type]);
}
