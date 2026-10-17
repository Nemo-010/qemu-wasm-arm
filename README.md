# qemu-wasm-arm

`qemu-system-arm` compiled to WebAssembly, running `arm-softmmu` in the browser,
with an SSD1306 OLED model so Zephyr display applications can be watched
running in a page.

**Live:** https://nemo-010.github.io/qemu-wasm-arm/

The page boots a real Zephyr application on an emulated `mps2-an385`
(Cortex-M3), and shows the panel by reading the emulated controller's own
GDDRAM — not a screenshot of a rendered surface, and not the guest's own claim
about what it drew.

---

## What is here

| Path | What it is |
| --- | --- |
| `web/` | The page, the wasm build, and a prebuilt Zephyr firmware |
| `patches/` | The changes to QEMU, including the new SSD1306 device |
| `zephyr-app/` | The Zephyr application the page boots |
| `test/smoke.js` | Checks the panel by reading it back out of the emulator |
| `build.sh` | What was actually run to produce the wasm build |

## What works

- QEMU 8.2 `arm-softmmu` running as wasm32, from `ktock/qemu-wasm`'s TCG backend.
- `mps2-an385` machine: Cortex-M3, CMSDK APB UART, SBCON I²C, SysTick.
- Zephyr boots, initialises devices, runs the scheduler, sleeps, and prints.
- An SSD1306 on the shield I²C bus probes successfully, and the Zephyr
  `ssd1306` driver writes real frames into it.
- The page renders those frames at 128×64 and streams the serial console.

## What does not work, and why

- **Tickless idle (`CONFIG_TICKLESS_KERNEL=y`) hangs.** With the default
  tickless configuration the kernel spins inside `sys_clock_elapsed()` and never
  reaches `main`. Setting `CONFIG_TICKLESS_KERNEL=n` makes it boot and keep
  time correctly. This is not worked around in QEMU; it is documented, and the
  demo firmware uses the tick-based configuration. Measured timing is right:
  three 200 ms sleeps landed at 120, 400 and 610 ms.
- **The build needs `-O0`-era workarounds.** See the gotchas below.
- **Only `arm-softmmu` is built.** No other targets, no networking, no
  graphics frontend.
- **No audio, no USB, no SD.** `--without-default-features` plus a deliberately
  small device set.

## The SSD1306 model

QEMU ships an SSD0303 model but nothing for the SSD1306, which is the panel
almost every small Zephyr and LVGL application targets. `patches/ssd1306.c` adds
one. It decodes the command stream the Zephyr driver actually emits:

- memory addressing mode (horizontal, vertical, page)
- column and page address windows
- display on/off, inversion, entire-display-on
- contrast, charge pump, multiplex ratio, clock divide, COM pins

and keeps a 1 KiB GDDRAM. The image is exposed two ways: as a `gddram` QOM
property (hex string) and through `ssd1306_get_framebuffer()`, both of which
read the controller's memory rather than a rendered surface.

The machine creates the panel on the first shield connector itself, because
`mps2.c` marks the first two I²C buses full and a `-device` on them fails with
an unhelpful "bus not found".

## The wasm32 address bug

QEMU's wasm32 TCG backend passed guest addresses to the MMU helpers with stray
high bits set. Zephyr faulted with:

```
***** BUS FAULT *****
  Precise data bus error
  BFAR Address: 0xe000e018
```

and QEMU's own log said:

```
Invalid read at addr 0x1E000E018, size 4, region '(null)', reason: rejected
```

The `1` in `0x1E000E018` is the bug: a 32-bit guest address had been
sign-extended into 64 bits, so the MMU looked for a page above the 32-bit space
and rejected the access. `patches/0001-...` truncates the address to 32 bits
before the helper call, for both loads and stores, which is what a 32-bit target
does anyway.

Without that fix the firmware boots, initialises every device, and then dies the
first time the timer is read.

## Building

The build is not a single command, because a number of things fail in
misleading ways. `build.sh` is the sequence that was actually run.

Prerequisites:

- emscripten 4.0.9 (`emsdk`), and `meson`/`ninja`
- a wasm32 build of glib 2.75, zlib 1.3.1, libffi 3.4.6, pcre2 10.42 under
  `$TARGET`, with `.pc` files it can find
- a checkout of [`ktock/qemu-wasm`](https://github.com/ktock/qemu-wasm)
  (QEMU 8.2 plus `tcg/wasm32`)

Then:

```sh
QEMU_SRC=/path/to/qemu-wasm TARGET=/path/to/target ./build.sh
```

### Gotchas, so nobody rediscovers them

- **`block.syms` / `qemu.syms` are generated empty** for this configuration and
  emcc will not expand a nested `@file`, so the link fails with
  `@block.syms: No such file or directory`. Drop the references.
- **`CONFIG_POSIX_MEMALIGN` is misdetected.** meson probes with
  `__builtin_posix_memalign`, which emscripten's clang lacks, so the probe says
  no and `util/memalign.c` refuses to compile. Force the define.
- **`getloadavg` collides** between emscripten's libc (with `-pthread`) and
  QEMU's static fallback. Guard the fallback on `__EMSCRIPTEN__`.
- **Atomics flags must be on every compile**, not only the link, or the linker
  rejects objects that use shared memory.
- **binaryen 117 does not know two flags emscripten 4.0.9 passes** to `wasm-opt`
  (`--enable-bulk-memory-opt`, `--enable-call-indirect-overlong`). Every
  optimised link dies with "Unknown option" pointing at the optimiser rather
  than at the version mismatch. A shim that drops just those two is in
  `build.sh`.
- **`-sENVIRONMENT` is link-only**, and glib builds with `-Werror`.
- **`BINARYEN` is deprecated** in favour of `EM_BINARYEN_ROOT`, but still works.

## Testing

```sh
node test/smoke.js
```

It boots the firmware in the emulator, waits, reads the GDDRAM, and checks:

- the framebuffer is 128×64 bits
- the guest printed something
- the panel is not blank
- all four edges are a solid border
- the interior contains exactly the 24×16 block the firmware draws
- nothing was written outside the GDDRAM

All of those are measured from the emulated controller, so they fail if the
display path breaks rather than if a file is merely missing.

## The Zephyr application

`zephyr-app/` builds for `mps2/an385` and puts an SSD1306 on `i2c_shield0`:

```sh
west build -b mps2/an385 zephyr-app
```

It draws a border and a block that moves, which is enough for the tests to tell
a real picture from a blank or scrambled one.

One detail worth knowing: the SSD1306 driver forwards the buffer straight to the
controller, so the buffer must be in the controller's own layout — one byte per
eight **vertical** pixels, page-major, top pixel in bit 0. Using the horizontal
layout that `display_write()` allows for other panels draws a transposed
picture.

## Credits

The wasm32 TCG backend is [ktock/qemu-wasm](https://github.com/ktock/qemu-wasm).
This repository adds the SSD1306 model, the address fix, the machine wiring and
the page.
