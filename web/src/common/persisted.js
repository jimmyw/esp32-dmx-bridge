import { useEffect, useState } from 'preact/hooks';

// useState that survives reloads via localStorage (per browser). Storage may be unavailable
// (private mode, blocked site data), so every access is guarded and falls back to `initial`.
export function usePersisted(key, initial) {
  const [value, setValue] = useState(() => {
    try {
      const s = localStorage.getItem(key);
      return s === null ? initial : JSON.parse(s);
    } catch (e) {
      return initial;
    }
  });
  useEffect(() => {
    try { localStorage.setItem(key, JSON.stringify(value)); } catch (e) {}
  }, [key, value]);
  return [value, setValue];
}
