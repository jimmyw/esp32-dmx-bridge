// Small fetch helpers for the bridge's JSON API.

export async function getJson(url) {
  const r = await fetch(url);
  return r.json();
}

export async function postJson(url, body) {
  const r = await fetch(url, {
    method: 'POST',
    headers: { 'Content-Type': 'application/json' },
    body: JSON.stringify(body),
  });
  return r.json();
}
