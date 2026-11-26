// The probe's service worker: adds `Authorization: Bearer <OneLake token>` to each request for
// onelake.dfs.fabric.microsoft.com that has none, as OneLake Studio's does. DuckDB-Wasm's HTTP
// requests (httpfs, from its worker) pass through here, so OneLake's files are read with the
// page's token. The page keeps the token in Cache Storage, which outlives an idle worker.
const ONELAKE_HOST = 'onelake.dfs.fabric.microsoft.com';

self.addEventListener('install', () => self.skipWaiting());
self.addEventListener('activate', event => event.waitUntil(self.clients.claim()));

self.addEventListener('fetch', event => {
  const url = new URL(event.request.url);
  if (url.hostname !== ONELAKE_HOST || event.request.headers.has('Authorization')) return;
  event.respondWith((async () => {
    const stored = await (await caches.open('onelake-token')).match('/__onelake_token');
    const headers = new Headers(event.request.headers);
    if (stored) headers.set('Authorization', `Bearer ${await stored.text()}`);
    return fetch(new Request(event.request, { headers, mode: 'cors' }));
  })());
});
