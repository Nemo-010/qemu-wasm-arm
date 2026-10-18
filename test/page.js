#!/usr/bin/env node
/*
 * Checks the page itself, not just the emulator.
 *
 * The first version of the page did nothing when you pressed "Boot firmware":
 * the build uses pthreads, pthreads need SharedArrayBuffer, and browsers only
 * expose that to a cross-origin isolated page. GitHub Pages cannot send the
 * headers that isolation requires, so the whole thing failed silently.
 *
 * These checks are static, because no headless browser is available here, but
 * they cover the things that actually broke: the service worker must exist and
 * inject the right headers, the page must gate the emulator on isolation, and
 * every failure path must write something the user can see.
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const WEB = path.join(ROOT, 'web');

let failures = 0;
function check(name, ok, detail) {
  console.log(`${ok ? 'ok  ' : 'FAIL'} ${name}${detail === undefined ? '' : ` — ${detail}`}`);
  if (!ok) failures++;
}

const html = fs.readFileSync(path.join(WEB, 'index.html'), 'utf8');
const swPath = path.join(WEB, 'coi-serviceworker.js');

check('service worker file exists', fs.existsSync(swPath));

const sw = fs.existsSync(swPath) ? fs.readFileSync(swPath, 'utf8') : '';
check('service worker sets COEP',
      /Cross-Origin-Embedder-Policy['"]?\s*,\s*['"]require-corp/.test(sw));
check('service worker sets COOP',
      /Cross-Origin-Opener-Policy['"]?\s*,\s*['"]same-origin/.test(sw));
check('service worker skips opaque responses',
      /status\s*===\s*0/.test(sw));

check('page registers the service worker',
      /serviceWorker\.register\(\s*['"]coi-serviceworker\.js['"]/.test(html));
check('page tests for isolation before loading the emulator',
      /crossOriginIsolated/.test(html));
check('page checks SharedArrayBuffer too',
      /SharedArrayBuffer/.test(html));

// The emulator must not be loaded by a plain <script src> in the markup, because
// that runs before isolation is known to be in place.
check('emulator is not loaded unconditionally',
      !/<script[^>]+src=["']qemu-system-arm\.js["']/.test(html));

// Every path that can fail must say so. This is the actual regression: silence.
check('boot() reports a missing emulator',
      /emulator script is not loaded/.test(html));
check('boot failures reach the status line',
      /boot failed:/.test(html));
check('script load failure is reported',
      /failed to load qemu-system-arm\.js/.test(html));

// The artifacts the page fetches must be present and non-trivial.
for (const [name, minBytes] of [['qemu-system-arm.js', 50000],
                                ['qemu-system-arm.wasm', 1000000],
                                ['firmware.bin', 10000]]) {
  const p = path.join(WEB, name);
  const size = fs.existsSync(p) ? fs.statSync(p).size : 0;
  check(`${name} is present`, size >= minBytes, `${size} bytes`);
}

console.log(failures === 0 ? '\nPASS' : `\nFAIL (${failures})`);
process.exit(failures === 0 ? 0 : 1);
