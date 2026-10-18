/*
 * Injects the cross-origin isolation headers GitHub Pages cannot send.
 *
 * The QEMU build uses pthreads, so it needs SharedArrayBuffer. Browsers only
 * expose SharedArrayBuffer to a cross-origin isolated page, and a page only
 * becomes isolated if the server sends:
 *
 *   Cross-Origin-Opener-Policy: same-origin
 *   Cross-Origin-Embedder-Policy: require-corp
 *
 * GitHub Pages has no way to set response headers, so this service worker
 * fetches each resource itself and re-serves it with those headers attached.
 * Once it is active the page reloads once and `crossOriginIsolated` is true.
 */

self.addEventListener('install', () => self.skipWaiting());

self.addEventListener('activate', (event) => {
  event.waitUntil(self.clients.claim());
});

self.addEventListener('fetch', (event) => {
  const request = event.request;

  // A cache-only request cannot be satisfied by refetching, so leave it alone.
  if (request.cache === 'only-if-cached' && request.mode !== 'same-origin') {
    return;
  }

  event.respondWith(
    fetch(request)
      .then((response) => {
        // Opaque responses have immutable headers, so they cannot be rewritten.
        if (response.status === 0) {
          return response;
        }

        const headers = new Headers(response.headers);
        headers.set('Cross-Origin-Embedder-Policy', 'require-corp');
        headers.set('Cross-Origin-Opener-Policy', 'same-origin');
        headers.set('Cross-Origin-Resource-Policy', 'cross-origin');

        return new Response(response.body, {
          status: response.status,
          statusText: response.statusText,
          headers,
        });
      })
      .catch((error) => {
        // Let the network error surface as a normal failed request.
        throw error;
      })
  );
});
