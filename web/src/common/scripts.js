import { postJson } from './api';

// Effect scripts API (main/script_web.c).
export const runScript = name => postJson('/api/scripts', { action: 'run', name });
export const stopScript = () => postJson('/api/scripts', { action: 'stop' });
export const deleteScript = name => postJson('/api/scripts', { action: 'delete', name });
export const setParams = params => postJson('/api/scripts', { params });

export async function loadSource(name) {
  const r = await fetch(`/scripts/${name}.js`, { cache: 'no-store' });
  if (!r.ok) throw new Error('not found');
  return r.text();
}

export async function saveSource(name, src) {
  const r = await fetch(`/scripts/${name}.js`, { method: 'PUT', body: src });
  return r.json();
}

export const validName = n => /^[A-Za-z0-9_-]{1,24}$/.test(n);
