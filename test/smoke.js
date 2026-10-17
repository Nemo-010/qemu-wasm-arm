#!/usr/bin/env node
/*
 * Verifies the browser runtime against a real emulator run.
 *
 * Everything here is measured from the emulated SSD1306's GDDRAM, not from a
 * screenshot or from the guest's own claims. If the display path breaks, the
 * pixels change and these assertions fail.
 *
 * Usage: node test/smoke.js [path/to/firmware.elf]
 */

const fs = require('fs');
const path = require('path');

const ROOT = path.resolve(__dirname, '..');
const W = 128;
const H = 64;

const firmwarePath = process.argv[2] || path.join(ROOT, 'web', 'firmware.bin');

let failures = 0;
function check(name, ok, detail) {
  const mark = ok ? 'ok  ' : 'FAIL';
  console.log(`${mark} ${name}${detail === undefined ? '' : ` — ${detail}`}`);
  if (!ok) failures++;
}

async function main() {
  const factory = require(path.join(ROOT, 'web', 'qemu-system-arm.js'));
  const firmware = fs.readFileSync(firmwarePath);
  const consoleLines = [];

  const module = await factory({
    noInitialRun: true,
    print: (s) => consoleLines.push(s),
    printErr: (s) => { if (!/unsupported syscall/.test(s)) consoleLines.push(s); },
  });

  module.FS.writeFile('/firmware.elf', new Uint8Array(firmware));
  module.callMain(['-machine', 'mps2-an385', '-kernel', '/firmware.elf',
                   '-display', 'none', '-serial', 'stdio']);

  // Let the guest boot, initialise the panel and draw a few frames.
  await new Promise((r) => setTimeout(r, 7000));

  const size = module._ssd1306_framebuffer_size();
  const ptr = module._malloc(size);
  module._ssd1306_get_framebuffer(ptr, size);
  const gddram = new Uint8Array(module.HEAPU8.subarray(ptr, ptr + size)).slice();
  module._free(ptr);

  // Decode the controller's page layout into pixels.
  const px = new Uint8Array(W * H);
  for (let page = 0; page < H / 8; page++) {
    for (let x = 0; x < W; x++) {
      const bits = gddram[page * W + x];
      for (let bit = 0; bit < 8; bit++) {
        px[(page * 8 + bit) * W + x] = (bits >> bit) & 1;
      }
    }
  }

  const lit = px.reduce((a, b) => a + b, 0);
  const rowLit = (y) => [...Array(W)].reduce((a, _, x) => a + px[y * W + x], 0);
  const colLit = (x) => [...Array(H)].reduce((a, _, y) => a + px[y * W + x], 0);

  check('framebuffer is 128x64 bits', size === 1024, `${size} bytes`);
  check('guest produced console output', consoleLines.length > 0, `${consoleLines.length} lines`);
  check('panel is not blank', lit > 0, `${lit} lit pixels`);

  // The firmware draws a one-pixel border around the whole panel.
  check('top row is a full border', rowLit(0) === W, `${rowLit(0)}/${W}`);
  check('bottom row is a full border', rowLit(H - 1) === W, `${rowLit(H - 1)}/${W}`);
  check('left column is a border', colLit(0) === H, `${colLit(0)}/${H}`);
  check('right column is a border', colLit(W - 1) === H, `${colLit(W - 1)}/${H}`);

  // ...and a 24x16 block somewhere inside, which is what makes it a picture.
  const interior = lit - (2 * W + 2 * (H - 2));
  check('interior has the moving block', interior === 24 * 16, `${interior} px, want ${24 * 16}`);

  // Nothing should have been drawn outside the panel's addressable area.
  const beyond = gddram.slice(1024).reduce((a, b) => a + b, 0);
  check('no writes outside GDDRAM', beyond === 0, `${beyond}`);

  console.log(failures === 0 ? '\nPASS' : `\nFAIL (${failures})`);
  process.exit(failures === 0 ? 0 : 1);
}

main().catch((e) => {
  console.error(e);
  process.exit(1);
});
