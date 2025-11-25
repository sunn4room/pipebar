#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <pixman.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/poll.h>
#include <sys/signalfd.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>
#include <wayland-client.h>

#include "protocols/wlr-layer-shell.h"
#include "protocols/xdg-shell.h"

#define VERSION "0.1.1"
#define CHAR_SIZE 1024
#define BLOCK_SIZE 32

enum {
    CODE_FOR_NORMAL = 0,
    CODE_FOR_INNER_ERROR = 1,
    CODE_FOR_RUNTIME_ERROR = 2,
};

enum {
    BLOCK_LEFT = 0,
    BLOCK_CENTER = 1,
    BLOCK_RIGHT = 2,
};

struct block {
    pixman_color_t bg;
    pixman_color_t fg;
    struct fcft_font *font;
    const char *action1;
    const char *action2;
    const char *action3;
    const char *action4;
    const char *action5;
    const char *action6;
    const char *action7;
    struct fcft_text_run *text;
    uint32_t width;
    uint32_t x;
    struct block *next;
};

struct wl_output_wrapper {
    struct wl_output *wl_output;
    uint32_t wl_output_name;
    const char *wl_output_real_name;
    struct bar *bar;
};

struct bar {
    const char *const version;

    pixman_color_t bg;
    pixman_color_t fg;
    struct fcft_font *font;
    const char *output;
    int gap;
    bool bottom;

    struct wl_display *wl_display;
    struct wl_registry *wl_registry;

    struct wl_compositor *wl_compositor;
    uint32_t wl_compositor_name;
    bool wl_output_available;
    struct wl_output_wrapper *wl_output_wrapper;
    struct wl_shm *wl_shm;
    uint32_t wl_shm_name;
    struct wl_seat *wl_seat;
    uint32_t wl_seat_name;
    struct wl_pointer *wl_pointer;
    struct zwlr_layer_shell_v1 *zwlr_layer_shell;
    uint32_t zwlr_layer_shell_name;
    struct wl_surface *wl_surface;
    struct zwlr_layer_surface_v1 *zwlr_layer_surface;

    uint32_t x;
    uint32_t y;
    uint32_t time;
    uint32_t throttle;
    uint32_t width;
    uint32_t height;
    void *mmapped;
    pixman_image_t *canvas[2];
    struct wl_buffer *wl_buffer[2];
    int released;

    char *chars;
    char *chars_in_reading;
    struct block *blocks;
    struct block **block;

    int signal_fd;
    int wl_display_fd;
    int stdin_fd;
};

static void cleanup_res(struct bar *bar) {

    if (bar->mmapped) {
        pixman_image_unref(bar->canvas[0]);
        pixman_image_unref(bar->canvas[1]);
        wl_buffer_destroy(bar->wl_buffer[0]);
        wl_buffer_destroy(bar->wl_buffer[1]);
        munmap(bar->mmapped, bar->width * bar->height * 4 * 2);
    }
    if (bar->zwlr_layer_surface) zwlr_layer_surface_v1_destroy(bar->zwlr_layer_surface);
    if (bar->wl_surface) wl_surface_destroy(bar->wl_surface);
    if (bar->zwlr_layer_shell) zwlr_layer_shell_v1_destroy(bar->zwlr_layer_shell);
    if (bar->wl_pointer) wl_pointer_destroy(bar->wl_pointer);
    if (bar->wl_seat) wl_seat_destroy(bar->wl_seat);
    if (bar->wl_shm) wl_shm_destroy(bar->wl_shm);
    if (bar->wl_output_wrapper) {
        wl_output_release(bar->wl_output_wrapper->wl_output);
        free(bar->wl_output_wrapper);
    }
    if (bar->wl_compositor) wl_compositor_destroy(bar->wl_compositor);
    if (bar->wl_registry) wl_registry_destroy(bar->wl_registry);
    if (bar->wl_display) wl_display_disconnect(bar->wl_display);
    if (bar->signal_fd != -1) close(bar->signal_fd);
    if (bar->wl_display_fd != -1) close(bar->wl_display_fd);
    if (bar->stdin_fd != -1) close(bar->stdin_fd);
}

static void die(struct bar *bar, int code, const char *fmt, ...) {

    if (fmt) {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);

        if (!fmt[0]) {
            perror(NULL);
        } else if (fmt[strlen(fmt) - 1] != '\n') {
            perror("");
        }
    }

    cleanup_res(bar);
    exit(code);
}

pixman_color_t strtocolor(const char *color_str) {
    uint32_t color_int = strtoul(color_str, NULL, 16);
    pixman_color_t color = {};
    color.alpha = (color_int >> 24 & 0xFF) * 0x0101;
    color.red = (color_int >> 16 & 0xFF) * 0x0101;
    color.green = (color_int >> 8 & 0xFF) * 0x0101;
    color.blue = (color_int & 0xFF) * 0x0101;
    return color;
}

static void draw(struct bar *bar, bool update) {

    if (update) {
        for (int i = 0; i < 3; i++) {
            for (struct block *block = bar->block[i]; block; block = block->next)
                fcft_text_run_destroy(block->text);
            bar->block[i] = NULL;
        }

        int blocks_idx = 0;
        int block_idx = BLOCK_LEFT;

        const struct block default_block = {
            .bg = bar->bg,
            .fg = bar->fg,
            .font = bar->font,
            .action1 = "",
            .action2 = "",
            .action3 = "",
            .action4 = "",
            .action5 = "",
            .action6 = "",
            .action7 = "",
        };
        struct block block = default_block;
        struct block mark = default_block;
        struct block *pre = NULL;

        char *reader = bar->chars;
        for (bool flag = false;; flag = !flag, reader = reader + strlen(reader) + 1) {
            if (flag) {
                if (reader[0] != 0) {
                    if (reader[0] == 'B') {
                        block.bg = reader[1] == 0 ? bar->bg : strtocolor(reader + 1);
                    } else if (reader[0] == 'F') {
                        block.fg = reader[1] == 0 ? bar->fg : strtocolor(reader + 1);
                    } else if (reader[0] == 'R') {
                        pixman_color_t tmp_color = block.fg;
                        block.fg = block.bg;
                        block.bg = tmp_color;
                    } else if (reader[0] == 'T') {
                        block.font = reader[1] == 0 ? bar->font : fcft_from_name(1, (const char *[]){reader + 1}, NULL);
                        if (block.font->height > bar->height) {
                            die(bar, CODE_FOR_RUNTIME_ERROR, "The font defined by control characters is too big: %s.\n", reader + 1);
                        }
                    } else if (reader[0] == '1') {
                        block.action1 = reader + 1;
                    } else if (reader[0] == '2') {
                        block.action2 = reader + 1;
                    } else if (reader[0] == '3') {
                        block.action3 = reader + 1;
                    } else if (reader[0] == '4') {
                        block.action4 = reader + 1;
                    } else if (reader[0] == '5') {
                        block.action5 = reader + 1;
                    } else if (reader[0] == '6') {
                        block.action6 = reader + 1;
                    } else if (reader[0] == '7') {
                        block.action7 = reader + 1;
                    } else if (reader[0] == 'D') {
                        block = default_block;
                    } else if (reader[0] == 'M') {
                        mark = block;
                    } else if (reader[0] == 'U') {
                        block = mark;
                    } else if (reader[0] == 'I') {
                        block_idx++;
                        if (block_idx > BLOCK_RIGHT) {
                            die(bar, CODE_FOR_RUNTIME_ERROR, "Too many I control characters.\n");
                        }
                        block = default_block;
                        mark = default_block;
                        pre = NULL;
                    } else {
                        die(bar, CODE_FOR_RUNTIME_ERROR, "Unkown control characters: %s.\n", reader);
                    }
                } else {
                    break;
                }
            } else {
                if (reader[0] != 0) {
                    if (blocks_idx == BLOCK_SIZE) {
                        die(bar, CODE_FOR_RUNTIME_ERROR, "BLOCK overflow.\n");
                    }
                    bar->blocks[blocks_idx] = block;
                    if (pre) {
                        pre->next = bar->blocks + blocks_idx;
                        pre = bar->blocks + blocks_idx;
                    } else {
                        pre = bar->blocks + blocks_idx;
                        bar->block[block_idx] = pre;
                    }

                    uint32_t unicodes[CHAR_SIZE] = {};
                    int reader_idx = 0;
                    int writer_idx = 0;
                    while (reader[reader_idx] != 0) {
                        if ((reader[reader_idx] & 0b10000000) == 0b00000000) {
                            unicodes[writer_idx] = reader[reader_idx];
                            reader_idx += 1;
                        } else if ((reader[reader_idx] & 0b11100000) == 0b11000000) {
                            unicodes[writer_idx] = ((reader[reader_idx] & 0b11111) << 6) | (reader[reader_idx + 1] & 0b111111);
                            reader_idx += 2;
                        } else if ((reader[reader_idx] & 0b11110000) == 0b11100000) {
                            unicodes[writer_idx] = ((reader[reader_idx] & 0b1111) << 12) | ((reader[reader_idx + 1] & 0b111111) << 6) | (reader[reader_idx + 2] & 0b111111);
                            reader_idx += 3;
                        } else {
                            unicodes[writer_idx] = ((reader[reader_idx] & 0b111) << 18) | ((reader[reader_idx + 1] & 0b111111) << 12) | ((reader[reader_idx + 2] & 0b111111) << 6) | (reader[reader_idx + 3] & 0b111111);
                            reader_idx += 4;
                        }
                        writer_idx += 1;
                    }
                    bar->blocks[blocks_idx].text = fcft_rasterize_text_run_utf32(bar->blocks[blocks_idx].font, writer_idx, unicodes, FCFT_SUBPIXEL_DEFAULT);
                    for (int j = 0; j < bar->blocks[blocks_idx].text->count; j++) {
                        bar->blocks[blocks_idx].width += bar->blocks[blocks_idx].text->glyphs[j]->advance.x;
                    }

                    blocks_idx++;
                }
            }
        }
    }

    pixman_box32_t bar_box = {0, 0, bar->width, bar->height};
    pixman_image_fill_boxes(PIXMAN_OP_SRC, bar->canvas[bar->released], &bar->bg, 1, &bar_box);
    pixman_image_t *fg_image = pixman_image_create_solid_fill(&bar->fg);
    for (int i = 0; i < 3; i++) {
        uint32_t total_width = 0;
        for (struct block *block = bar->block[i]; block; block = block->next)
            total_width += block->width;
        if (total_width == 0) continue;
        uint32_t x = i == BLOCK_LEFT ? 0 : (i == BLOCK_RIGHT ? (bar->width - total_width) : ((bar->width - total_width) / 2));
        for (struct block *block = bar->block[i]; block; block = block->next) {
            block->x = x;
            if (memcmp(&block->bg, &bar->bg, sizeof(pixman_color_t))) {
                pixman_box32_t block_box = {block->x, (bar->height - block->font->height) / 2, block->x + block->width, (bar->height + block->font->height) / 2};
                pixman_image_fill_boxes(PIXMAN_OP_SRC, bar->canvas[bar->released], &block->bg, 1, &block_box);
            }
            pixman_image_t *block_fg_image;
            if (memcmp(&block->fg, &bar->fg, sizeof(pixman_color_t))) {
                block_fg_image = pixman_image_create_solid_fill(&block->fg);
            } else {
                block_fg_image = pixman_image_ref(fg_image);
            }
            int baseline = (block->font->height + block->font->descent + block->font->ascent) / 2 - (block->font->descent > 0 ? block->font->descent : 0) + (bar->height - block->font->height) / 2;
            for (int j = 0; j < block->text->count; j++) {
                const struct fcft_glyph *glyph = block->text->glyphs[j];
                if (glyph->is_color_glyph) {
                    pixman_image_composite32(PIXMAN_OP_OVER, glyph->pix, NULL, bar->canvas[bar->released], 0, 0, 0, 0, x + glyph->x, baseline - glyph->y, glyph->width, glyph->height);
                } else {
                    pixman_image_composite32(PIXMAN_OP_OVER, block_fg_image, glyph->pix, bar->canvas[bar->released], 0, 0, 0, 0, x + glyph->x, baseline - glyph->y, glyph->width, glyph->height);
                }
                x += glyph->advance.x;
            }
            pixman_image_unref(block_fg_image);
        }
    }
    pixman_image_unref(fg_image);

    wl_surface_attach(bar->wl_surface, bar->wl_buffer[bar->released], 0, 0);
    wl_surface_damage(bar->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(bar->wl_surface);
    bar->released = -1;
}

static void randname(char *buf) {
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;
    for (int i = 0; i < 6; ++i) {
        buf[i] = 'A' + (r & 15) + (r & 16) * 2;
        r >>= 5;
    }
}

static int create_shm_file(void) {
    int retries = 100;
    do {
        char name[] = "/wl_shm-XXXXXX";
        randname(name + sizeof(name) - 7);
        --retries;
        int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
        if (fd >= 0) {
            shm_unlink(name);
            return fd;
        }
    } while (retries > 0 && errno == EEXIST);
    return -1;
}

static int allocate_shm_file(size_t size) {
    int fd = create_shm_file();
    if (fd < 0)
        return -1;
    int ret;
    do {
        ret = ftruncate(fd, size);
    } while (ret < 0 && errno == EINTR);
    if (ret < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

static void wl_buffer_handle_release(void *data, struct wl_buffer *wl_buffer) {

    struct bar *bar = data;
    if (wl_buffer == bar->wl_buffer[0])
        bar->released = 0;
    else if (wl_buffer == bar->wl_buffer[1])
        bar->released = 1;
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_handle_release,
};

static void zwlr_layer_surface_handle_configure(void *data, struct zwlr_layer_surface_v1 *zwlr_layer_surface, uint32_t serial, uint32_t width, uint32_t height) {

    zwlr_layer_surface_v1_ack_configure(zwlr_layer_surface, serial);
    struct bar *bar = data;

    int fd = allocate_shm_file(width * height * 4 * 2);
    if (fd == -1) die(bar, CODE_FOR_INNER_ERROR, "Failed to allocate shm file");
    void *mmapped = mmap(NULL, width * height * 4 * 2, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mmapped == MAP_FAILED) die(bar, CODE_FOR_INNER_ERROR, "Failed to map shm file");
    struct wl_shm_pool *pool = wl_shm_create_pool(bar->wl_shm, fd, width * height * 4 * 2);

    if (bar->mmapped) {
        pixman_image_unref(bar->canvas[0]);
        pixman_image_unref(bar->canvas[1]);
        wl_buffer_destroy(bar->wl_buffer[0]);
        wl_buffer_destroy(bar->wl_buffer[1]);
        munmap(bar->mmapped, bar->width * bar->height * 4 * 2);
    }
    bar->width = width;
    bar->height = height;
    bar->mmapped = mmapped;
    bar->canvas[0] = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, bar->mmapped, bar->width * 4);
    bar->canvas[1] = pixman_image_create_bits(PIXMAN_a8r8g8b8, width, height, bar->mmapped + bar->width * bar->height * 4, bar->width * 4);
    bar->wl_buffer[0] = wl_shm_pool_create_buffer(pool, 0, bar->width, bar->height, bar->width * 4, WL_SHM_FORMAT_ARGB8888);
    bar->wl_buffer[1] = wl_shm_pool_create_buffer(pool, bar->width * bar->height * 4, bar->width, bar->height, bar->width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_buffer_add_listener(bar->wl_buffer[0], &wl_buffer_listener, bar);
    wl_buffer_add_listener(bar->wl_buffer[1], &wl_buffer_listener, bar);
    bar->released = 0;
    draw(bar, false);
    bar->released = 1;

    wl_shm_pool_destroy(pool);
    close(fd);
}

static void zwlr_layer_surface_handle_closed(void *data, struct zwlr_layer_surface_v1 *zwlr_layer_surface_v1) {

    struct bar *bar = data;
    die(bar, CODE_FOR_NORMAL, NULL);
}

static const struct zwlr_layer_surface_v1_listener zwlr_layer_surface_listener = {
    .configure = zwlr_layer_surface_handle_configure,
    .closed = zwlr_layer_surface_handle_closed,
};

static void wl_output_handle_name(void *data, struct wl_output *wl_output, const char *name) {

    struct wl_output_wrapper *wrapper = data;
    wrapper->wl_output_real_name = name;
}

static void wl_output_handle_done(void *data, struct wl_output *wl_output) {

    struct wl_output_wrapper *wrapper = data;
    if (!wrapper->bar->wl_output_wrapper && (!wrapper->bar->output || !strcmp(wrapper->bar->output, wrapper->wl_output_real_name))) {
        struct bar *bar = wrapper->bar;
        bar->wl_output_wrapper = wrapper;
        bar->wl_surface = wl_compositor_create_surface(bar->wl_compositor);
        bar->zwlr_layer_surface = zwlr_layer_shell_v1_get_layer_surface(bar->zwlr_layer_shell, bar->wl_surface, bar->wl_output_wrapper->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "statusbar");
        zwlr_layer_surface_v1_add_listener(bar->zwlr_layer_surface, &zwlr_layer_surface_listener, bar);
        zwlr_layer_surface_v1_set_anchor(bar->zwlr_layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | (bar->bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP));
        zwlr_layer_surface_v1_set_margin(bar->zwlr_layer_surface, bar->gap, bar->gap, bar->gap, bar->gap);
    }
    if (wrapper != wrapper->bar->wl_output_wrapper) {
        wl_output_release(wrapper->wl_output);
        free(wrapper);
    } else {
        struct bar *bar = wrapper->bar;
        zwlr_layer_surface_v1_set_exclusive_zone(bar->zwlr_layer_surface, bar->font->height);
        zwlr_layer_surface_v1_set_size(bar->zwlr_layer_surface, 0, bar->font->height);
        wl_surface_commit(bar->wl_surface);
    }
}

static void wl_output_handle_geometry(void *data, struct wl_output *wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char *make, const char *model, int32_t transform) {}
static void wl_output_handle_mode(void *data, struct wl_output *wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) {}
static void wl_output_handle_scale(void *data, struct wl_output *wl_output, int32_t factor) {}
static void wl_output_handle_description(void *data, struct wl_output *wl_output, const char *description) {}
static const struct wl_output_listener wl_output_listener = {
    .name = wl_output_handle_name,
    .geometry = wl_output_handle_geometry,
    .mode = wl_output_handle_mode,
    .done = wl_output_handle_done,
    .scale = wl_output_handle_scale,
    .description = wl_output_handle_description,
};

static void wl_pointer_handle_enter(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface, wl_fixed_t surface_x, wl_fixed_t surface_y) {

    struct bar *bar = data;
    bar->x = wl_fixed_to_double(surface_x);
    bar->y = wl_fixed_to_double(surface_y);
}

static void wl_pointer_handle_leave(void *data, struct wl_pointer *wl_pointer, uint32_t serial, struct wl_surface *surface) {

    struct bar *bar = data;
    bar->x = UINT32_MAX;
    bar->y = UINT32_MAX;
}

static void wl_pointer_handle_motion(void *data, struct wl_pointer *wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y) {

    struct bar *bar = data;
    bar->x = wl_fixed_to_double(surface_x);
    bar->y = wl_fixed_to_double(surface_y);
}

static void wl_pointer_handle_button(void *data, struct wl_pointer *wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state) {

    struct bar *bar = data;

    if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;
    if (time - bar->time < bar->throttle)
        return;
    else
        bar->time = time;

    for (int i = 0; i < 3; i++) {
        for (struct block *block = bar->block[i]; block; block = block->next) {
            if (bar->x < block->x) {
                break;
            } else if (bar->x < block->x + block->width) {
                if (bar->y > (bar->height - block->font->height) / 2 && bar->y < (bar->height + block->font->height) / 2) {
                    if (button == BTN_LEFT && block->action1[0] != 0) {
                        fprintf(stdout, "%s\n", block->action1);
                    } else if (button == BTN_MIDDLE && block->action2[0] != 0) {
                        fprintf(stdout, "%s\n", block->action2);
                    } else if (button == BTN_RIGHT && block->action3[0] != 0) {
                        fprintf(stdout, "%s\n", block->action3);
                    }
                }
                return;
            }
        }
    }
}

static void wl_pointer_handle_axis(void *data, struct wl_pointer *wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value) {

    struct bar *bar = data;
    if (time - bar->time < bar->throttle)
        return;
    else
        bar->time = time;

    for (int i = 0; i < 3; i++) {
        for (struct block *block = bar->block[i]; block; block = block->next) {
            if (bar->x < block->x) {
                break;
            } else if (bar->x < block->x + block->width) {
                if (bar->y > (bar->height - block->font->height) / 2 && bar->y < (bar->height + block->font->height) / 2) {
                    if (axis == 0 && value > 0 && block->action4[0] != 0) {
                        fprintf(stdout, "%s\n", block->action4);
                    } else if (axis == 0 && value < 0 && block->action5[0] != 0) {
                        fprintf(stdout, "%s\n", block->action5);
                    } else if (axis == 1 && value > 0 && block->action6[0] != 0) {
                        fprintf(stdout, "%s\n", block->action6);
                    } else if (axis == 1 && value < 0 && block->action7[0] != 0) {
                        fprintf(stdout, "%s\n", block->action7);
                    }
                }
                return;
            }
        }
    }
}

static const struct wl_pointer_listener wl_pointer_listener = {
    .enter = wl_pointer_handle_enter,
    .leave = wl_pointer_handle_leave,
    .motion = wl_pointer_handle_motion,
    .button = wl_pointer_handle_button,
    .axis = wl_pointer_handle_axis,
};

static void wl_seat_handle_capabilities(void *data, struct wl_seat *wl_seat, uint32_t capabilities) {

    struct bar *bar = data;
    if (capabilities & WL_SEAT_CAPABILITY_POINTER) {
        bar->wl_pointer = wl_seat_get_pointer(bar->wl_seat);
        wl_pointer_add_listener(bar->wl_pointer, &wl_pointer_listener, bar);
    }
}

static const struct wl_seat_listener wl_seat_listener = {
    .capabilities = wl_seat_handle_capabilities,
};

static void wl_registry_handle_global(void *data, struct wl_registry *wl_registry, uint32_t name, const char *interface, uint32_t version) {

    struct bar *bar = data;
    if (!strcmp(interface, wl_compositor_interface.name)) {
        bar->wl_compositor = wl_registry_bind(wl_registry, name, &wl_compositor_interface, 4);
        bar->wl_compositor_name = name;
    } else if (!strcmp(interface, wl_output_interface.name)) {
        bar->wl_output_available = true;
        struct wl_output_wrapper *wrapper = calloc(1, sizeof(struct wl_output_wrapper));
        wrapper->wl_output = wl_registry_bind(wl_registry, name, &wl_output_interface, 4);
        wrapper->wl_output_name = name;
        wrapper->bar = bar;
        wl_output_add_listener(wrapper->wl_output, &wl_output_listener, wrapper);
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        bar->wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 1);
        bar->wl_shm_name = name;
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        bar->wl_seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 1);
        bar->wl_seat_name = name;
        wl_seat_add_listener(bar->wl_seat, &wl_seat_listener, bar);
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        bar->zwlr_layer_shell = wl_registry_bind(wl_registry, name, &zwlr_layer_shell_v1_interface, 3);
        bar->zwlr_layer_shell_name = name;
    }
}

static void wl_registry_handle_global_remove(void *data, struct wl_registry *wl_registry, uint32_t name) {

    struct bar *bar = data;
    if (name == bar->wl_compositor_name || name == bar->wl_output_wrapper->wl_output_name || name == bar->wl_shm_name || name == bar->wl_seat_name || name == bar->zwlr_layer_shell_name) {
        die(bar, CODE_FOR_NORMAL, NULL);
    }
}

static const struct wl_registry_listener wl_registry_listener = {
    .global = wl_registry_handle_global,
    .global_remove = wl_registry_handle_global_remove,
};

static void init() {

    struct stat stdin_stat;
    fstat(STDIN_FILENO, &stdin_stat);
    if (!S_ISFIFO(stdin_stat.st_mode)) {
        fprintf(stderr, "Pipebar is a Wayland statusbar reading content from STDIN and sending event action to STDOUT.\n");
        fprintf(stderr, "pipebar version %s\n", VERSION);
        fprintf(stderr, "\n");
        fprintf(stderr, "usage: <producer> | pipebar [options] | <consumer>\n");
        fprintf(stderr, "options are:\n");
        fprintf(stderr, "        -B <aarrggbb>   set default background color (FF000000)\n");
        fprintf(stderr, "        -F <aarrggbb>   set default foreground color (FFFFFFFF)\n");
        fprintf(stderr, "        -T <font>       set default font (monospace)\n");
        fprintf(stderr, "        -o <output>     put the bar at the special output monitor\n");
        fprintf(stderr, "        -b              put the bar at the bottom\n");
        fprintf(stderr, "        -g <gap>        set margin gap (0)\n");
        fprintf(stderr, "        -t <ms>         set action throttle time in ms (500)\n");
        fprintf(stderr, "\n");
        fprintf(stderr, "producer can produce control characters between a pair of \\x1f.\n");
        fprintf(stderr, "control characters are:\n");
        fprintf(stderr, "        Baarrggbb       set background color to aarrggbb\n");
        fprintf(stderr, "        B               set background color to default\n");
        fprintf(stderr, "        Faarrggbb       set foreground color to aarrggbb\n");
        fprintf(stderr, "        F               set foreground color to default\n");
        fprintf(stderr, "        R               swap foreground and background\n");
        fprintf(stderr, "        Tname:k=v       set font to name:k=v\n");
        fprintf(stderr, "        T               set font to default\n");
        fprintf(stderr, "        1action         print action for consumer when click left button\n");
        fprintf(stderr, "        2action         print action for consumer when click middle button\n");
        fprintf(stderr, "        3action         print action for consumer when click right button\n");
        fprintf(stderr, "        4action         print action for consumer when scroll axis down\n");
        fprintf(stderr, "        5action         print action for consumer when scroll axis up\n");
        fprintf(stderr, "        6action         print action for consumer when scroll axis left\n");
        fprintf(stderr, "        7action         print action for consumer when scroll axis right\n");
        fprintf(stderr, "        n               close action 1/2/3/4/5/6/7 area\n");
        fprintf(stderr, "        M               mark the current control state\n");
        fprintf(stderr, "        U               restore current control state to the marked\n");
        fprintf(stderr, "        D               restore current control state to default\n");
        fprintf(stderr, "        I               the delimeter between left/center and center/right block\n");
        fprintf(stderr, "\n");
        exit(CODE_FOR_NORMAL);
    }

    fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_ERROR);
    if (!(fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING)) {
        fprintf(stderr, "Font does not support text-run shaping.\n");
        exit(CODE_FOR_INNER_ERROR);
    }

    if (!getenv("WAYLAND_DISPLAY")) {
        fprintf(stderr, "$WAYLAND_DISPLAY is NULL.\n");
        exit(CODE_FOR_INNER_ERROR);
    }
}

static void override(struct bar *bar, int argc, char **argv) {

    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "-B")) {
            if (++i < argc) bar->bg = strtocolor(argv[i]);
        } else if (!strcmp(argv[i], "-F")) {
            if (++i < argc) bar->fg = strtocolor(argv[i]);
        } else if (!strcmp(argv[i], "-T")) {
            if (++i < argc) bar->font = fcft_from_name(1, (const char *[]){argv[i]}, NULL);
        } else if (!strcmp(argv[i], "-o")) {
            if (++i < argc) bar->output = argv[i];
        } else if (!strcmp(argv[i], "-g")) {
            if (++i < argc) bar->gap = strtol(argv[i], NULL, 10);
        } else if (!strcmp(argv[i], "-t")) {
            if (++i < argc) bar->throttle = strtol(argv[i], NULL, 10);
        } else if (!strcmp(argv[i], "-b")) {
            bar->bottom = true;
        } else {
            die(bar, CODE_FOR_RUNTIME_ERROR, "Unknown option: %s", argv[i]);
        }
    }
}

static void setup(struct bar *bar) {

    bar->wl_display = wl_display_connect(NULL);
    if (!bar->wl_display) {
        die(bar, CODE_FOR_INNER_ERROR, "Failed to connect to wayland display.\n");
    }

    bar->wl_registry = wl_display_get_registry(bar->wl_display);
    wl_registry_add_listener(bar->wl_registry, &wl_registry_listener, bar);
    wl_display_roundtrip(bar->wl_display);
    if (!bar->wl_compositor || !bar->wl_output_available || !bar->wl_shm || !bar->wl_seat || !bar->zwlr_layer_shell) {
        die(bar, CODE_FOR_INNER_ERROR, "Failed to adapt to wayland display.\n");
    }
    wl_display_roundtrip(bar->wl_display);
    if (!bar->wl_output_wrapper) {
        die(bar, CODE_FOR_INNER_ERROR, "Failed to find the wayland output %s.\n", bar->output);
    }
    wl_display_roundtrip(bar->wl_display);
}

static void run(struct bar *bar) {

    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        die(bar, CODE_FOR_INNER_ERROR, "Failed to intercept signal");
    }
    bar->signal_fd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (bar->signal_fd < 0) {
        die(bar, CODE_FOR_INNER_ERROR, "Failed to create signal fd");
    }

    bar->wl_display_fd = wl_display_get_fd(bar->wl_display);
    if (bar->wl_display_fd < 0) {
        die(bar, CODE_FOR_INNER_ERROR, "Failed to get wayland display fd");
    }

    bar->stdin_fd = STDIN_FILENO;
    char *reader = bar->chars_in_reading;
    int sep_count = 0;

    struct pollfd pfds[] = {
        {.fd = bar->signal_fd, .events = POLLIN},
        {.fd = bar->wl_display_fd, .events = POLLIN},
        {.fd = bar->stdin_fd, .events = POLLIN},
    };

    bool redraw = false;
    while (true) {
        wl_display_flush(bar->wl_display);

        while (poll(pfds, 3, -1) < 0) {
            die(bar, CODE_FOR_INNER_ERROR, "Failed to wait for data from fds using poll");
        }

        if (pfds[0].revents & POLLIN) {
            die(bar, CODE_FOR_NORMAL, "Interrupted by signal.\n");
        }

        if (pfds[1].revents & POLLIN) {
            if (wl_display_dispatch(bar->wl_display) < 0) {
                die(bar, CODE_FOR_INNER_ERROR, "Failed to dispatch wayland events");
            }
        }

        if (pfds[2].revents & POLLHUP) {
            die(bar, CODE_FOR_NORMAL, "STDIN EOF.\n");
        }

        if (pfds[2].revents & POLLIN) {
            read(bar->stdin_fd, reader, 1);
            if (reader[0] == '\n') {
                if (sep_count % 2 == 0) {
                    reader[0] = 0;
                    reader[1] = 0;
                    char *chars_tmp = bar->chars;
                    bar->chars = bar->chars_in_reading;
                    bar->chars_in_reading = chars_tmp;
                    reader = bar->chars_in_reading;
                    sep_count = 0;
                    redraw = true;
                } else {
                    die(bar, CODE_FOR_RUNTIME_ERROR, "An odd number of 0x1f was found in stdin line.\n");
                }
            } else {
                if (reader[0] == 0x1f) {
                    reader[0] = 0;
                    sep_count++;
                    if (sep_count % 2 == 0 && reader[-1] == 0) {
                        die(bar, CODE_FOR_RUNTIME_ERROR, "Empty is no allowed between a pair of 0x1f.\n");
                    }
                }
                if (reader + 3 >= bar->chars_in_reading + CHAR_SIZE) {
                    die(bar, CODE_FOR_RUNTIME_ERROR, "Characters overflow.\n");
                } else {
                    reader++;
                }
            }
        }

        if (redraw && bar->released != -1) {
            draw(bar, true);
            redraw = false;
        }
    }
}

int main(int argc, char **argv) {

    init();

    struct bar bar = {
        .version = "0.0.1",
        .bg = {0x0000, 0x0000, 0x0000, 0xFFFF},
        .fg = {0xFFFF, 0xFFFF, 0xFFFF, 0xFFFF},
        .font = fcft_from_name(1, (const char *[]){"monospace"}, NULL),
        .output = NULL,
        .bottom = false,
        .gap = 0,
        .throttle = 500,
        .chars = (char[CHAR_SIZE]){},
        .chars_in_reading = (char[CHAR_SIZE]){},
        .blocks = (struct block[BLOCK_SIZE]){},
        .block = (struct block *[3]){},
        .signal_fd = -1,
        .wl_display_fd = -1,
        .stdin_fd = -1,
    };

    override(&bar, argc, argv);
    setup(&bar);
    run(&bar);
}
