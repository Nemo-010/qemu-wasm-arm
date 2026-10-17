# Patches to QEMU

These apply on top of [`ktock/qemu-wasm`](https://github.com/ktock/qemu-wasm)
(QEMU 8.2 with the wasm32 TCG backend).

- `0001-ssd1306-and-wasm32-address-fixes.patch` — the SSD1306 model, the
  machine wiring, and the wasm32 guest-address truncation fix.
- `ssd1306.c` — the new device in full, for reading without applying a patch.

Apply with:

```sh
cd qemu-wasm
git apply /path/to/patches/0001-ssd1306-and-wasm32-address-fixes.patch
```

The patch touches:

| File | Why |
| --- | --- |
| `hw/display/ssd1306.c` | new: the SSD1306 device |
| `hw/display/Kconfig` | `CONFIG_SSD1306` |
| `hw/display/meson.build` | build the new file |
| `hw/arm/Kconfig` | `mps2` selects `SSD1306` |
| `hw/arm/mps2.c` | create the panel on the first shield I²C bus |
| `include/qemu/osdep.h` | don't define `getloadavg` under emscripten |
| `tcg/wasm32/tcg-target.c.inc` | truncate guest addresses to 32 bits |
