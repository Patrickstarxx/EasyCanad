import { createHash } from 'node:crypto';
import { readdir, writeFile } from 'node:fs/promises';
import { join } from 'node:path';
import { fileURLToPath } from 'node:url';

const root = new URL('../dist/', import.meta.url);
const assets = await readdir(new URL('assets/', root));
const paths = ['./index.html', './manifest.webmanifest', './icon.svg', './icon-192.png', './icon-512.png',
  ...assets.map(name => `./assets/${name}`)];
const hash = createHash('sha256').update(paths.join('\n')).digest('hex').slice(0, 10);
const source = `const CACHE = 'easycanad-host-${hash}';
const PRECACHE = ${JSON.stringify(paths)};
self.addEventListener('install', event => {
  event.waitUntil(caches.open(CACHE).then(cache => cache.addAll(PRECACHE)));
  self.skipWaiting();
});
self.addEventListener('activate', event => {
  event.waitUntil(caches.keys().then(keys => Promise.all(keys.filter(key => key.startsWith('easycanad-host-') && key !== CACHE).map(key => caches.delete(key)))));
  self.clients.claim();
});
self.addEventListener('fetch', event => {
  if (event.request.method !== 'GET' || new URL(event.request.url).origin !== self.location.origin) return;
  event.respondWith(fetch(event.request).then(response => {
    if (response.ok) {
      const copy = response.clone();
      void caches.open(CACHE).then(cache => cache.put(event.request, copy));
    }
    return response;
  }).catch(() => caches.match(event.request).then(cached => cached ||
    (event.request.mode === 'navigate' ? caches.match('./index.html') : Response.error()))));
});
`;
await writeFile(join(fileURLToPath(root), 'sw.js'), source);
console.log(`Offline cache: ${paths.length} files`);
