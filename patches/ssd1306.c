/*
 * Solomon SSD1306 128x64 monochrome OLED controller.
 *
 * QEMU has an SSD0303 model but nothing for the SSD1306, which is the panel
 * most small Zephyr and LVGL applications target. This model decodes the real
 * command stream the Zephyr driver emits (horizontal/vertical/page addressing,
 * column and page windows, contrast, charge pump, inversion) and keeps a GDDRAM
 * image that can be read back.
 *
 * The framebuffer is exposed through a "data" memory region so a host can pull
 * the pixels out without a display frontend. That is what the browser page and
 * the test harness use: the panel contents are read as bytes, not screenshotted.
 *
 * This code is licensed under the GPL, like the rest of QEMU.
 */

#include "qemu/osdep.h"
#include "hw/i2c/i2c.h"
#include "migration/vmstate.h"
#include "qemu/module.h"
#include "qom/object.h"

#define TYPE_SSD1306 "ssd1306"
OBJECT_DECLARE_SIMPLE_TYPE(SSD1306State, SSD1306)

#define SSD1306_WIDTH   128
#define SSD1306_HEIGHT  64
#define SSD1306_PAGES   (SSD1306_HEIGHT / 8)
#define SSD1306_GDDRAM   (SSD1306_WIDTH * SSD1306_PAGES)

/* Control byte in the I2C payload selects command or data. */
#define SSD1306_CTRL_CMD    0x00
#define SSD1306_CTRL_DATA   0x40
#define SSD1306_CTRL_MASK   0xc0

/* Addressing modes. */
#define SSD1306_ADDR_HORIZONTAL 0
#define SSD1306_ADDR_VERTICAL   1
#define SSD1306_ADDR_PAGE       2

struct SSD1306State {
    I2CSlave parent_obj;

    uint8_t gddram[SSD1306_GDDRAM];
    uint8_t col;
    uint8_t page;
    uint8_t col_start;
    uint8_t col_end;
    uint8_t page_start;
    uint8_t page_end;
    uint8_t addr_mode;
    bool charge_pump;
    bool enabled;
    bool inverse;
    bool entire_on;

    /* Multi-byte command arguments arrive as separate I2C bytes. */
    uint8_t pending;
    uint8_t pending_args;
    uint8_t pending_buf[2];

    /* Latched control byte: 0x00 commands, 0x40 data. */
    uint8_t ctrl;
    bool have_ctrl;
};

static void ssd1306_update_window(SSD1306State *s)
{
    s->col = s->col_start;
    s->page = s->page_start;
}

static void ssd1306_exec(SSD1306State *s, uint8_t cmd)
{
    switch (cmd) {
    case 0x00 ... 0x0f:             /* lower column address */
        s->col = (s->col & 0xf0) | (cmd & 0x0f);
        break;
    case 0x10 ... 0x1f:             /* higher column address */
        s->col = (s->col & 0x0f) | ((cmd & 0x0f) << 4);
        break;
    case 0x20:                      /* memory addressing mode */
        s->pending = cmd;
        s->pending_args = 1;
        break;
    case 0x21:                      /* column address window */
        s->pending = cmd;
        s->pending_args = 2;
        break;
    case 0x22:                      /* page address window */
        s->pending = cmd;
        s->pending_args = 2;
        break;
    case 0x26:                      /* horizontal scroll: 6 args, ignored */
    case 0x27:
    case 0x29:
    case 0x2a:
        s->pending = cmd;
        s->pending_args = 6;
        break;
    case 0x2e:                      /* stop scrolling */
    case 0x2f:                      /* start scrolling */
        break;
    case 0x40 ... 0x7f:             /* display start line */
        break;
    case 0x81:                      /* contrast */
        s->pending = cmd;
        s->pending_args = 1;
        break;
    case 0xa0 ... 0xa1:             /* segment remap */
        break;
    case 0xa4:                      /* resume from RAM content */
        s->entire_on = false;
        break;
    case 0xa5:                      /* entire display on */
        s->entire_on = true;
        break;
    case 0xa6:                      /* normal */
        s->inverse = false;
        break;
    case 0xa7:                      /* inverted */
        s->inverse = true;
        break;
    case 0xa8:                      /* multiplex ratio */
    case 0xd3:                      /* display offset */
    case 0xd5:                      /* clock divide */
    case 0xd9:                      /* pre-charge period */
    case 0xda:                      /* COM pins */
    case 0xdb:                      /* VCOMH deselect */
        s->pending = cmd;
        s->pending_args = 1;
        break;
    case 0x8d:                      /* charge pump */
        s->pending = cmd;
        s->pending_args = 1;
        break;
    case 0xae:                      /* display off */
        s->enabled = false;
        break;
    case 0xaf:                      /* display on */
        s->enabled = true;
        break;
    case 0xb0 ... 0xb7:             /* page start address (page mode) */
        s->page = cmd & 0x07;
        break;
    case 0xc0 ... 0xc8:             /* COM scan direction */
    case 0xe3:                      /* nop */
        break;
    default:
        break;
    }
}

static void ssd1306_command_arg(SSD1306State *s, uint8_t arg)
{
    uint8_t idx = s->pending_args;

    /* Collect arguments in order. */
    if (s->pending == 0x21 || s->pending == 0x22) {
        s->pending_buf[2 - idx] = arg;
    }

    s->pending_args--;

    if (s->pending_args) {
        return;
    }

    switch (s->pending) {
    case 0x20:
        s->addr_mode = arg & 0x03;
        ssd1306_update_window(s);
        break;
    case 0x21:
        s->col_start = s->pending_buf[0] & 0x7f;
        s->col_end = s->pending_buf[1] & 0x7f;
        if (s->col_end >= SSD1306_WIDTH) {
            s->col_end = SSD1306_WIDTH - 1;
        }
        if (s->col_start > s->col_end) {
            s->col_start = s->col_end;
        }
        ssd1306_update_window(s);
        break;
    case 0x22:
        s->page_start = s->pending_buf[0] & 0x07;
        s->page_end = s->pending_buf[1] & 0x07;
        if (s->page_end >= SSD1306_PAGES) {
            s->page_end = SSD1306_PAGES - 1;
        }
        if (s->page_start > s->page_end) {
            s->page_start = s->page_end;
        }
        ssd1306_update_window(s);
        break;
    case 0x8d:
        s->charge_pump = (arg & 0x04) != 0;
        break;
    default:
        break;
    }
    s->pending = 0;
}

static void ssd1306_data(SSD1306State *s, uint8_t data)
{
    s->gddram[s->page * SSD1306_WIDTH + s->col] = data;

    switch (s->addr_mode) {
    case SSD1306_ADDR_HORIZONTAL:
        if (s->col >= s->col_end) {
            s->col = s->col_start;
            if (s->page >= s->page_end) {
                s->page = s->page_start;
            } else {
                s->page++;
            }
        } else {
            s->col++;
        }
        break;
    case SSD1306_ADDR_VERTICAL:
        if (s->page >= s->page_end) {
            s->page = s->page_start;
            if (s->col >= s->col_end) {
                s->col = s->col_start;
            } else {
                s->col++;
            }
        } else {
            s->page++;
        }
        break;
    default:                        /* page addressing */
        if (s->col >= SSD1306_WIDTH - 1) {
            s->col = 0;
        } else {
            s->col++;
        }
        break;
    }
}

static int ssd1306_send(I2CSlave *i2c, uint8_t data)
{
    SSD1306State *s = SSD1306(i2c);

    /*
     * Each I2C transfer starts with a control byte: 0x00 means every following
     * byte is a command, 0x40 means every following byte is GDDRAM data. Zephyr
     * sends one control byte per transfer, so the first byte after START sets
     * the mode and the rest are payload.
     */
    if (!s->have_ctrl) {
        s->have_ctrl = true;
        if (data == SSD1306_CTRL_CMD || data == 0x80) {
            s->ctrl = SSD1306_CTRL_CMD;
        } else if (data == SSD1306_CTRL_DATA) {
            s->ctrl = SSD1306_CTRL_DATA;
        } else {
            /* No control byte: treat the byte as a command, as the panel does. */
            s->ctrl = SSD1306_CTRL_CMD;
            s->have_ctrl = true;
            ssd1306_exec(s, data);
            return 0;
        }
        return 0;
    }

    if (s->pending_args) {
        ssd1306_command_arg(s, data);
        return 0;
    }

    if (s->ctrl == SSD1306_CTRL_DATA) {
        ssd1306_data(s, data);
    } else {
        ssd1306_exec(s, data);
    }
    return 0;
}

static int ssd1306_event(I2CSlave *i2c, enum i2c_event event)
{
    SSD1306State *s = SSD1306(i2c);

    switch (event) {
    case I2C_START_SEND:
    case I2C_START_RECV:
        s->have_ctrl = false;
        break;
    case I2C_FINISH:
    case I2C_NACK:
        break;
    }
    return 0;
}

/*
 * The GDDRAM is exposed as a hex string so a host can read the panel contents
 * with qom-get and decode the pixels itself. This is deliberately not a
 * screenshot: it is the controller memory the driver actually wrote, which is
 * what makes the readback a real check on the display path.
 */
static char *ssd1306_get_gddram(Object *obj, Error **errp)
{
    SSD1306State *s = SSD1306(obj);
    char *hex = g_malloc(sizeof(s->gddram) * 2 + 1);

    for (size_t i = 0; i < sizeof(s->gddram); i++) {
        sprintf(hex + i * 2, "%02x", s->gddram[i]);
    }
    hex[sizeof(s->gddram) * 2] = '\0';
    return hex;
}

static SSD1306State *ssd1306_last;

void ssd1306_get_framebuffer(uint8_t *dst, size_t len)
{
    if (!ssd1306_last) {
        memset(dst, 0, len);
        return;
    }
    memcpy(dst, ssd1306_last->gddram, MIN(len, sizeof(ssd1306_last->gddram)));
}

size_t ssd1306_framebuffer_size(void)
{
    return SSD1306_GDDRAM;
}

static void ssd1306_realize(DeviceState *dev, Error **errp)
{
    SSD1306State *s = SSD1306(dev);

    ssd1306_last = s;

    s->addr_mode = SSD1306_ADDR_PAGE;
    s->col_end = SSD1306_WIDTH - 1;
    s->page_end = SSD1306_PAGES - 1;
    s->enabled = true;
    object_property_add_str(OBJECT(dev), "gddram",
                            ssd1306_get_gddram, NULL);
}

static void ssd1306_reset(DeviceState *dev)
{
    SSD1306State *s = SSD1306(dev);

    memset(s->gddram, 0, sizeof(s->gddram));
    s->col = 0;
    s->page = 0;
    s->col_start = 0;
    s->col_end = SSD1306_WIDTH - 1;
    s->page_start = 0;
    s->page_end = SSD1306_PAGES - 1;
    s->addr_mode = SSD1306_ADDR_PAGE;
    s->pending_args = 0;
    s->pending = 0;
}

static const VMStateDescription vmstate_ssd1306 = {
    .name = TYPE_SSD1306,
    .version_id = 1,
    .minimum_version_id = 1,
    .fields = (const VMStateField[]) {
        VMSTATE_UINT8_ARRAY(gddram, SSD1306State, SSD1306_GDDRAM),
        VMSTATE_END_OF_LIST()
    }
};

static void ssd1306_class_init(ObjectClass *klass, void *data)
{
    DeviceClass *dc = DEVICE_CLASS(klass);
    I2CSlaveClass *k = I2C_SLAVE_CLASS(klass);

    k->event = ssd1306_event;
    k->send = ssd1306_send;
    dc->realize = ssd1306_realize;
    dc->reset = ssd1306_reset;
    dc->vmsd = &vmstate_ssd1306;
    set_bit(DEVICE_CATEGORY_DISPLAY, dc->categories);
}

static const TypeInfo ssd1306_info = {
    .name = TYPE_SSD1306,
    .parent = TYPE_I2C_SLAVE,
    .instance_size = sizeof(SSD1306State),
    .class_init = ssd1306_class_init,
};

static void ssd1306_register_types(void)
{
    type_register_static(&ssd1306_info);
}

type_init(ssd1306_register_types)
