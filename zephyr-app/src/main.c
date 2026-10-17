/*
 * A Zephyr application for the emulated machine.
 *
 * It proves the browser QEMU runs real Zephyr firmware: it prints on the
 * emulated console and draws on an SSD1306 over the shield I2C bus, so both the
 * serial path and the display path are exercised.
 *
 * It loops forever rather than returning, which is what a display application
 * does anyway.
 */

#include <zephyr/kernel.h>
#include <zephyr/drivers/display.h>
#include <zephyr/device.h>
#include <stdio.h>
#include <string.h>

#define WIDTH  128
#define HEIGHT 64

int main(void)
{
	const struct device *display = DEVICE_DT_GET(DT_CHOSEN(zephyr_display));
	static uint8_t buf[WIDTH * HEIGHT / 8];

	printk("qemu-wasm-arm: Zephyr booted on the emulated Cortex-M\n");

	if (!device_is_ready(display)) {
		printk("qemu-wasm-arm: display not ready\n");
		return 0;
	}

	printk("qemu-wasm-arm: display %s ready\n", display->name);
	display_blanking_off(display);

	/*
	 * A border plus a filled block that moves, so the captured GDDRAM has
	 * structure a test can assert on rather than a blank screen.
	 */
	for (int frame = 0;; frame++) {
		memset(buf, 0, sizeof(buf));

		/*
		 * The SSD1306 driver forwards the buffer straight to the controller, so
		 * the buffer must already be in the controller's own layout: one byte per
		 * eight VERTICAL pixels, page-major, with the top pixel in bit 0.
		 *
		 *   index = (y / 8) * WIDTH + x
		 *   bit   = y % 8
		 *
		 * Using the horizontal layout instead produces a transposed picture, which
		 * is exactly what the first version of this app drew.
		 */
		for (int x = 0; x < WIDTH; x++) {
			buf[0 * WIDTH + x] |= BIT(0);
			buf[(HEIGHT / 8 - 1) * WIDTH + x] |= BIT(7);
		}
		for (int y = 0; y < HEIGHT; y++) {
			buf[(y / 8) * WIDTH + 0] |= BIT(y % 8);
			buf[(y / 8) * WIDTH + (WIDTH - 1)] |= BIT(y % 8);
		}

		int bx = (frame * 4) % (WIDTH - 24);
		for (int y = 24; y < 40; y++) {
			for (int x = bx; x < bx + 24; x++) {
				buf[(y / 8) * WIDTH + x] |= BIT(y % 8);
			}
		}

		struct display_buffer_descriptor desc = {
			.buf_size = sizeof(buf),
			.width = WIDTH,
			.height = HEIGHT,
			.pitch = WIDTH,
		};

		display_write(display, 0, 0, &desc, buf);
		printk("qemu-wasm-arm: frame %d\n", frame);
		k_sleep(K_MSEC(250));
	}

	return 0;
}
