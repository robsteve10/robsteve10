// Tiny JSON client for the bridge's HTTP API.
async function call(method, path, body, timeoutMs = 15000) {
  const ctl = new AbortController();
  const t = setTimeout(() => ctl.abort(), timeoutMs);
  try {
    const res = await fetch(path, {
      method,
      headers: body ? { 'Content-Type': 'application/json' } : undefined,
      body: body ? JSON.stringify(body) : undefined,
      cache: 'no-store',
      signal: ctl.signal,
    });
    let data = {};
    try { data = await res.json(); } catch { /* empty */ }
    if (!res.ok) throw new Error(data.error || `Bridge answered ${res.status}`);
    return data;
  } catch (e) {
    if (e.name === 'AbortError') throw new Error('The bridge did not answer in time');
    if (e instanceof TypeError) throw new Error('Cannot reach the bridge. Is it running, and is this phone on the same Wi-Fi?');
    throw e;
  } finally {
    clearTimeout(t);
  }
}

export const api = {
  get: (path) => call('GET', path),
  post: (path, body = {}, timeoutMs) => call('POST', path, body, timeoutMs),
};
