#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <signal.h>
#include <stdarg.h>
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

#include "protocols/fractional-scale.h"
#include "protocols/viewporter.h"
#include "protocols/wlr-layer-shell.h"

enum {
    WARNING = -1,
    NO_ERROR = 0,
    INNER_ERROR = 1,
    RUNTIME_ERROR = 2,
};

enum {
    PART_LEFT,
    PART_CENTER,
    PART_RIGHT,
    PART_SIZE,
};

struct fcft_font_wrapper {
    char* name;
    struct fcft_font* font;
};

static void fcft_font_wrapper_destroy(struct fcft_font_wrapper* fw)
{
    fcft_destroy(fw->font);
}

struct wl_output_wrapper {
    struct wl_output* wl_output;
    uint32_t wl_name;
    const char* name;
    struct bar* bar;
};

static void wl_output_wrapper_destroy(struct wl_output_wrapper* ow)
{
    wl_output_release(ow->wl_output);
}

struct wl_seat_wrapper {
    struct wl_seat* wl_seat;
    uint32_t wl_name;
    const char* name;
    struct bar* bar;
};

static void wl_seat_wrapper_destroy(struct wl_seat_wrapper* sw)
{
    wl_seat_release(sw->wl_seat);
}

struct wl_buffer_wrapper {
    struct wl_buffer* wl_buffer;
    uint32_t width, height;
    void* mmapped;
    struct wl_list link;
    struct bar* bar;
};

static void wl_buffer_wrapper_destroy(struct wl_buffer_wrapper* bw)
{
    wl_buffer_destroy(bw->wl_buffer);
    munmap(bw->mmapped, bw->width * bw->height * 4);
}

struct block;

struct item {
    const char* value;
    struct wl_list* last;
};

enum {
    ITEM_BG,
    ITEM_FG,
    ITEM_FONT,
    ITEM_ACT1,
    ITEM_ACT2,
    ITEM_ACT3,
    ITEM_ACT4,
    ITEM_ACT5,
    ITEM_ACT6,
    ITEM_ACT7,
    ITEM_SIZE,
};

struct block {
    struct item item[ITEM_SIZE];
    const char* text;

    struct fcft_text_run* run;
    uint32_t width, height, x, y, base;

    struct wl_list link;
};

static void block_destroy(struct block* block)
{
    fcft_text_run_destroy(block->run);
}

struct bar {
    const char* version;

    char* colors;
    struct wl_array color;
    char* fonts;
    struct wl_array font;
    char* output;
    char* seat;
    bool bottom;
    uint32_t gap;
    uint32_t throttle;
    char* repstr;

    struct wl_display* wl_display;
    struct wl_registry* wl_registry;

    struct wl_compositor* wl_compositor;
    uint32_t wl_compositor_name;
    struct wl_shm* wl_shm;
    uint32_t wl_shm_name;
    struct wp_fractional_scale_manager_v1* wp_fractional_scale_manager;
    uint32_t wp_fractional_scale_manager_name;
    struct wp_viewporter* wp_viewporter;
    uint32_t wp_viewporter_name;
    struct zwlr_layer_shell_v1* zwlr_layer_shell;
    uint32_t zwlr_layer_shell_name;

    bool wl_output_available;
    struct wl_output_wrapper* wl_output_wrapper;
    struct wl_surface* wl_surface;
    struct wp_fractional_scale_v1* wp_fractional_scale;
    struct wp_viewport* wp_viewport;
    struct zwlr_layer_surface_v1* zwlr_layer_surface;

    bool wl_seat_available;
    struct wl_seat_wrapper* wl_seat_wrapper;
    struct wl_pointer* wl_pointer;

    struct wl_buffer_wrapper* wl_buffer_wrapper;
    struct wl_list wl_buffer_wrapper_busy;

    struct wl_list part[PART_SIZE];
    struct wl_list block;
    struct wl_array text;
    struct wl_array text_busy;
    struct wl_array codepoint;

    uint32_t width, height;
    uint32_t x, y;
    uint32_t scale;
    char dpi[16];
    uint32_t buf_width, buf_height;
    uint32_t time;
    bool redraw;
};

static void msg(struct bar* bar, const int code, const char* restrict fmt, ...)
{
    if (fmt != NULL && fmt[0] != '\0') {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }

    if (code == WARNING) return;

    struct block* block;
    for (int i = 0; i < PART_SIZE; i++) {
        wl_list_for_each(block, &bar->part[i], link)
        {
            block_destroy(block);
            free(block);
        }
    }
    wl_list_for_each(block, &bar->block, link)
    {
        free(block);
    }
    wl_array_release(&bar->text);
    wl_array_release(&bar->text_busy);
    wl_array_release(&bar->codepoint);
    if (bar->wl_buffer_wrapper != NULL) {
        wl_buffer_wrapper_destroy(bar->wl_buffer_wrapper);
        free(bar->wl_buffer_wrapper);
    }
    struct wl_buffer_wrapper* bw;
    wl_list_for_each(bw, &bar->wl_buffer_wrapper_busy, link)
    {
        wl_buffer_wrapper_destroy(bw);
        free(bw);
    }
    if (bar->wl_seat_wrapper != NULL) {
        if (bar->wl_pointer != NULL) wl_pointer_release(bar->wl_pointer);
        wl_seat_wrapper_destroy(bar->wl_seat_wrapper);
        free(bar->wl_seat_wrapper);
    }
    if (bar->wl_output_wrapper != NULL) {
        if (bar->zwlr_layer_surface != NULL) zwlr_layer_surface_v1_destroy(bar->zwlr_layer_surface);
        if (bar->wp_viewport != NULL) wp_viewport_destroy(bar->wp_viewport);
        if (bar->wp_fractional_scale != NULL) wp_fractional_scale_v1_destroy(bar->wp_fractional_scale);
        if (bar->wl_surface != NULL) wl_surface_destroy(bar->wl_surface);
        wl_output_wrapper_destroy(bar->wl_output_wrapper);
        free(bar->wl_output_wrapper);
    }
    if (bar->zwlr_layer_shell != NULL) zwlr_layer_shell_v1_destroy(bar->zwlr_layer_shell);
    if (bar->wp_viewporter != NULL) wp_viewporter_destroy(bar->wp_viewporter);
    if (bar->wp_fractional_scale_manager != NULL) wp_fractional_scale_manager_v1_destroy(bar->wp_fractional_scale_manager);
    if (bar->wl_shm != NULL) wl_shm_release(bar->wl_shm);
    if (bar->wl_compositor != NULL) wl_compositor_destroy(bar->wl_compositor);
    if (bar->wl_registry != NULL) wl_registry_destroy(bar->wl_registry);
    if (bar->wl_display != NULL) wl_display_disconnect(bar->wl_display);
    wl_array_release(&bar->color);
    struct fcft_font_wrapper* fw;
    wl_array_for_each(fw, &bar->font)
    {
        fcft_font_wrapper_destroy(fw);
    }
    wl_array_release(&bar->font);
    fcft_fini();
    exit(code);
}

static void wp_fractional_scale_handle_preferred_scale(void* data, struct wp_fractional_scale_v1* wp_fractional_scale_v1, uint32_t scale)
{
    struct bar* bar = data;
    bar->scale = scale;
    bar->buf_width = bar->width * bar->scale / 120;
    sprintf(bar->dpi, "dpi=%u", 96 * bar->scale / 120);
    bar->buf_height = 0;
    for (struct fcft_font_wrapper* fw = bar->font.data; (void*)fw < bar->font.data + bar->font.size; fw++) {
        fcft_destroy(fw->font);
        fw->font = fcft_from_name(1, (const char*[]) { fw->name }, bar->dpi);
        if (fw->font->height > bar->buf_height) {
            bar->buf_height = fw->font->height;
        }
    }
    bar->redraw = true;
}
static const struct wp_fractional_scale_v1_listener wp_fractional_scale_listener = {
    .preferred_scale = wp_fractional_scale_handle_preferred_scale,
};

static void zwlr_layer_surface_handle_configure(void* data, struct zwlr_layer_surface_v1* zwlr_layer_surface, uint32_t serial, uint32_t width, uint32_t height)
{
    zwlr_layer_surface_v1_ack_configure(zwlr_layer_surface, serial);
    struct bar* bar = data;
    bar->width = width;
    wp_viewport_set_destination(bar->wp_viewport, bar->width, bar->height);
    bar->buf_width = bar->width * bar->scale / 120;
    bar->redraw = true;
}
static void zwlr_layer_surface_handle_closed(void* data, struct zwlr_layer_surface_v1* zwlr_layer_surface_v1) { }
static const struct zwlr_layer_surface_v1_listener zwlr_layer_surface_listener = {
    .configure = zwlr_layer_surface_handle_configure,
    .closed = zwlr_layer_surface_handle_closed,
};

static void wl_output_handle_name(void* data, struct wl_output* wl_output, const char* name)
{
    struct wl_output_wrapper* ow = data;
    ow->name = name;
}
static void wl_output_handle_done(void* data, struct wl_output* wl_output)
{
    struct wl_output_wrapper* ow = data;
    struct bar* bar = ow->bar;
    if (bar->wl_output_wrapper == NULL && (bar->output == NULL || strcmp(ow->name, bar->output) == 0)) {
        bar->wl_output_wrapper = ow;

        bar->wl_surface = wl_compositor_create_surface(bar->wl_compositor);
        bar->wp_viewport = wp_viewporter_get_viewport(bar->wp_viewporter, bar->wl_surface);
        bar->wp_fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(bar->wp_fractional_scale_manager, bar->wl_surface);
        wp_fractional_scale_v1_add_listener(bar->wp_fractional_scale, &wp_fractional_scale_listener, bar);
        bar->zwlr_layer_surface = zwlr_layer_shell_v1_get_layer_surface(bar->zwlr_layer_shell, bar->wl_surface, bar->wl_output_wrapper->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "statusbar");
        zwlr_layer_surface_v1_add_listener(bar->zwlr_layer_surface, &zwlr_layer_surface_listener, bar);
        zwlr_layer_surface_v1_set_anchor(bar->zwlr_layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | (bar->bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP));
        zwlr_layer_surface_v1_set_margin(bar->zwlr_layer_surface, bar->gap, bar->gap, bar->gap, bar->gap);
        zwlr_layer_surface_v1_set_exclusive_zone(bar->zwlr_layer_surface, bar->height);
        zwlr_layer_surface_v1_set_size(bar->zwlr_layer_surface, 0, bar->height);
        wl_surface_commit(bar->wl_surface);
        return;
    }
    if (ow != bar->wl_output_wrapper) {
        wl_output_release(ow->wl_output);
        free(ow);
    } else {
        zwlr_layer_surface_v1_set_size(bar->zwlr_layer_surface, 0, bar->height);
    }
}
static void wl_output_handle_scale(void* data, struct wl_output* wl_output, int32_t factor) { }
static void wl_output_handle_geometry(void* data, struct wl_output* wl_output, int32_t x, int32_t y, int32_t physical_width, int32_t physical_height, int32_t subpixel, const char* make, const char* model, int32_t transform) { }
static void wl_output_handle_mode(void* data, struct wl_output* wl_output, uint32_t flags, int32_t width, int32_t height, int32_t refresh) { }
static void wl_output_handle_description(void* data, struct wl_output* wl_output, const char* description) { }
static const struct wl_output_listener wl_output_listener = {
    .name = wl_output_handle_name,
    .geometry = wl_output_handle_geometry,
    .mode = wl_output_handle_mode,
    .done = wl_output_handle_done,
    .scale = wl_output_handle_scale,
    .description = wl_output_handle_description,
};

static void print_action(struct bar* bar, struct block* block, int item_idx, uint32_t x)
{
    float f = -1; // lazy load
    unsigned long repstr_len = strlen(bar->repstr);
    for (const char* reader = block->item[item_idx].value; reader[0] != '\0'; reader += 1) {
        if (strncmp(reader, bar->repstr, repstr_len) == 0) {
            if (f < 0) {
                f = 0;
                struct block* each;
                wl_list_for_each_reverse(each, block->item[item_idx].last, link)
                {
                    if (each->run != NULL) f += each->run->count;
                    if (each == block) break;
                }
                uint32_t x_width = x - block->x;
                uint32_t width = block->width;
                for (int i = block->run->count - 1; i >= 0; i--) {
                    uint32_t glyph_width = block->run->glyphs[i]->advance.x;
                    if (width - glyph_width > x_width) {
                        width -= glyph_width;
                        f -= 1;
                    } else {
                        f -= (width - x_width) * 1.0 / glyph_width;
                        break;
                    }
                }
            }
            fprintf(stdout, "%f", f);
            reader += repstr_len - 1;
        } else {
            fputc(reader[0], stdout);
        }
    }
    fputc('\n', stdout);
}
static void wl_pointer_handle_enter(void* data, struct wl_pointer* wl_pointer, uint32_t serial, struct wl_surface* surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct bar* bar = data;
    bar->x = wl_fixed_to_double(surface_x);
    bar->y = wl_fixed_to_double(surface_y);
}
static void wl_pointer_handle_leave(void* data, struct wl_pointer* wl_pointer, uint32_t serial, struct wl_surface* surface)
{
    struct bar* bar = data;
    bar->x = UINT32_MAX;
    bar->y = UINT32_MAX;
}
static void wl_pointer_handle_motion(void* data, struct wl_pointer* wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct bar* bar = data;
    bar->x = wl_fixed_to_double(surface_x);
    bar->y = wl_fixed_to_double(surface_y);
}
static void wl_pointer_handle_button(void* data, struct wl_pointer* wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;

    struct bar* bar = data;
    if (bar->redraw) return;

    if (time - bar->time < bar->throttle) {
        return;
    } else {
        bar->time = time;
    }

    uint32_t x = bar->x * bar->buf_width / bar->width;
    uint32_t y = bar->y * bar->buf_height / bar->height;
    for (int part_idx = PART_LEFT; part_idx < PART_SIZE; part_idx++) {
        struct block* block;
        wl_list_for_each_reverse(block, &bar->part[part_idx], link)
        {
            if (x < block->x) {
                break;
            } else if (x < block->x + block->width) {
                if (y >= block->y && y < block->y + block->height) {
                    if (button == BTN_LEFT && block->item[ITEM_ACT1].value != NULL) {
                        print_action(bar, block, ITEM_ACT1, x);
                    } else if (button == BTN_MIDDLE && block->item[ITEM_ACT2].value != NULL) {
                        print_action(bar, block, ITEM_ACT2, x);
                    } else if (button == BTN_RIGHT && block->item[ITEM_ACT3].value != NULL) {
                        print_action(bar, block, ITEM_ACT3, x);
                    }
                }
                return;
            }
        }
    }
}
static void wl_pointer_handle_axis(void* data, struct wl_pointer* wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct bar* bar = data;
    if (bar->redraw) return;

    if (time - bar->time < bar->throttle) {
        return;
    } else {
        bar->time = time;
    }

    uint32_t x = bar->x * bar->buf_width / bar->width;
    uint32_t y = bar->y * bar->buf_height / bar->height;
    for (int part_idx = PART_LEFT; part_idx < PART_SIZE; part_idx++) {
        struct block* block;
        wl_list_for_each_reverse(block, &bar->part[part_idx], link)
        {
            if (x < block->x) {
                break;
            } else if (x < block->x + block->width) {
                if (y >= block->y && y < block->y + block->height) {
                    if (axis == 0 && value > 0 && block->item[ITEM_ACT4].value != NULL) {
                        print_action(bar, block, ITEM_ACT4, x);
                    } else if (axis == 0 && value < 0 && block->item[ITEM_ACT5].value != NULL) {
                        print_action(bar, block, ITEM_ACT5, x);
                    } else if (axis == 1 && value > 0 && block->item[ITEM_ACT6].value != NULL) {
                        print_action(bar, block, ITEM_ACT6, x);
                    } else if (axis == 1 && value < 0 && block->item[ITEM_ACT7].value != NULL) {
                        print_action(bar, block, ITEM_ACT7, x);
                    }
                }
                return;
            }
        }
    }
}
static void wl_pointer_handle_frame(void* data, struct wl_pointer* wl_pointer) { }
static void wl_pointer_handle_axis_source(void* data, struct wl_pointer* wl_pointer, uint32_t axis_source) { }
static void wl_pointer_handle_axis_stop(void* data, struct wl_pointer* wl_pointer, uint32_t time, uint32_t axis) { }
static void wl_pointer_handle_axis_discrete(void* data, struct wl_pointer* wl_pointer, uint32_t axis, int32_t discrete) { }
static const struct wl_pointer_listener wl_pointer_listener = {
    .enter = wl_pointer_handle_enter,
    .leave = wl_pointer_handle_leave,
    .motion = wl_pointer_handle_motion,
    .button = wl_pointer_handle_button,
    .axis = wl_pointer_handle_axis,
    .frame = wl_pointer_handle_frame,
    .axis_source = wl_pointer_handle_axis_source,
    .axis_stop = wl_pointer_handle_axis_stop,
    .axis_discrete = wl_pointer_handle_axis_discrete,
};

static void wl_seat_handle_name(void* data, struct wl_seat* wl_seat, const char* name)
{
    struct wl_seat_wrapper* sw = data;
    sw->name = name;
}
static void wl_seat_handle_capabilities(void* data, struct wl_seat* wl_seat, uint32_t capabilities)
{
    struct wl_seat_wrapper* sw = data;
    struct bar* bar = sw->bar;
    if (bar->wl_seat_wrapper == NULL && (bar->seat == NULL || strcmp(sw->name, bar->output) == 0)) {
        bar->wl_seat_wrapper = sw;
    }
    if (sw != bar->wl_seat_wrapper) {
        wl_seat_release(sw->wl_seat);
        free(sw);
    } else {
        bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
        if (have_pointer && bar->wl_pointer == NULL) {
            bar->wl_pointer = wl_seat_get_pointer(bar->wl_seat_wrapper->wl_seat);
            wl_pointer_add_listener(bar->wl_pointer, &wl_pointer_listener, bar);
        } else if (!have_pointer && bar->wl_pointer != NULL) {
            wl_pointer_release(bar->wl_pointer);
            bar->wl_pointer = NULL;
            bar->x = UINT32_MAX;
            bar->y = UINT32_MAX;
        }
    }
}
static const struct wl_seat_listener wl_seat_listener = {
    .name = wl_seat_handle_name,
    .capabilities = wl_seat_handle_capabilities,
};

static void wl_registry_handle_global(void* data, struct wl_registry* wl_registry, uint32_t name, const char* interface, uint32_t version)
{
    struct bar* bar = data;
    if (!strcmp(interface, wl_compositor_interface.name)) {
        bar->wl_compositor = wl_registry_bind(wl_registry, name, &wl_compositor_interface, 3);
        bar->wl_compositor_name = name;
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        bar->wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 2);
        bar->wl_shm_name = name;
    } else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
        bar->wp_fractional_scale_manager = wl_registry_bind(wl_registry, name, &wp_fractional_scale_manager_v1_interface, 1);
        bar->zwlr_layer_shell_name = name;
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        bar->wp_viewporter = wl_registry_bind(wl_registry, name, &wp_viewporter_interface, 1);
        bar->wp_viewporter_name = name;
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        bar->zwlr_layer_shell = wl_registry_bind(wl_registry, name, &zwlr_layer_shell_v1_interface, 3);
        bar->zwlr_layer_shell_name = name;
    } else if (!strcmp(interface, wl_output_interface.name)) {
        bar->wl_output_available = true;
        struct wl_output_wrapper* wl_output_wrapper = calloc(1, sizeof(struct wl_output_wrapper));
        wl_output_wrapper->wl_output = wl_registry_bind(wl_registry, name, &wl_output_interface, 4);
        wl_output_wrapper->wl_name = name;
        wl_output_wrapper->bar = bar;
        wl_output_add_listener(wl_output_wrapper->wl_output, &wl_output_listener, wl_output_wrapper);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        bar->wl_seat_available = true;
        struct wl_seat_wrapper* wl_seat_wrapper = calloc(1, sizeof(struct wl_seat_wrapper));
        wl_seat_wrapper->wl_seat = wl_registry_bind(wl_registry, name, &wl_seat_interface, 5);
        wl_seat_wrapper->wl_name = name;
        wl_seat_wrapper->bar = bar;
        wl_seat_add_listener(wl_seat_wrapper->wl_seat, &wl_seat_listener, wl_seat_wrapper);
    }
}
static void wl_registry_handle_global_remove(void* data, struct wl_registry* wl_registry, uint32_t name)
{
    struct bar* bar = data;
    if (name == bar->wl_compositor_name) {
        msg(bar, INNER_ERROR, "Wayland compositor removed.");
    } else if (name == bar->wl_shm_name) {
        msg(bar, INNER_ERROR, "Wayland shared memory removed.");
    } else if (name == bar->wp_fractional_scale_manager_name) {
        msg(bar, INNER_ERROR, "Wayland fractional scale manager removed.");
    } else if (name == bar->wp_viewporter_name) {
        msg(bar, INNER_ERROR, "Wayland viewporter removed.");
    } else if (name == bar->zwlr_layer_shell_name) {
        msg(bar, INNER_ERROR, "Wayland layer shell removed.");
    } else if (bar->wl_output_wrapper != NULL && name == bar->wl_output_wrapper->wl_name) {
        msg(bar, INNER_ERROR, "Wayland output removed.");
    } else if (bar->wl_seat_wrapper != NULL && name == bar->wl_seat_wrapper->wl_name) {
        msg(bar, INNER_ERROR, "Wayland seat removed.");
    }
}
static const struct wl_registry_listener wl_registry_listener = {
    .global = wl_registry_handle_global,
    .global_remove = wl_registry_handle_global_remove,
};

static void pipe_init(struct bar* bar)
{
    struct stat stdin_stat;
    fstat(STDIN_FILENO, &stdin_stat);
    if (!S_ISFIFO(stdin_stat.st_mode)) {
        msg(bar, NO_ERROR,
            "pbar is a featherweight text-rendering wayland statusbar.\n"
            "pbar renders utf-8 sequence from STDIN and prints pointer event actions to STDOUT.\n"
            "sequence between a pair of '\\x1f' will be escaped instead of being rendered directly.\n"
            "\n"
            "        version         %s\n"
            "        usage           producer | pbar [options] | consumer\n"
            "\n"
            "options are:\n"
            "        -c color,color  set colors list (000000ff,ffffffff)\n"
            "        -f font,font    set fonts list (monospace)\n"
            "        -o output       set wayland output\n"
            "        -s seat         set wayland seat\n"
            "        -b              place the bar at the bottom\n"
            "        -g gap          set margin gap (0)\n"
            "        -i interval     set pointer event throttle interval in ms (100)\n"
            "        -r rep_str      set the replace string for action ({})\n"
            "\n"
            "color can be:\n"
            "        rrggbb          without alpha\n"
            "        rrggbbaa        with alpha\n"
            "\n"
            "font can be: (see 'man fcft_from_name')\n"
            "        name            font name\n"
            "        name:k=v        with single attribute\n"
            "        name:k=v:k=v    with multiple attributes\n"
            "\n"
            "environment variable:\n"
            "        PBAR_COLORS     set colors list\n"
            "        PBAR_FONTS      set fonts list\n"
            "\n"
            "escape sequence can be:\n"
            "        Bindex          set background color index\n"
            "        B               restore last background color index\n"
            "        Findex          set foreground color index\n"
            "        F               restore last foreground color index\n"
            "        Tindex          set font index\n"
            "        T               restore last font index\n"
            "        1action         set left button click action\n"
            "        1               restore last left button click action\n"
            "        2action         set middle button click action\n"
            "        2               restore last middle button click action\n"
            "        3action         set right button click action\n"
            "        3               restore right button click action\n"
            "        4action         set axis scroll down action\n"
            "        4               restore last axis scroll down action\n"
            "        5action         set axis scroll up action\n"
            "        5               restore last axis scroll up action\n"
            "        6action         set axis scroll left action\n"
            "        6               restore last axis scroll left action\n"
            "        7action         set axis scroll right action\n"
            "        7               restore last axis scroll right action\n"
            "        R               swap background color and foreground color\n"
            "        D               delimiter between left/center and center/right part\n"
            "\n"
            "index can be:\n"
            "        0               the first item in colors/fonts list\n"
            "        1               the second item in colors/fonts list\n"
            "        ...             ...\n"
            "\n"
            "action can be:\n"
            "        xxx             anything except for '\\x1f'\n"
            "        xxx rep_str     rep_str will be replaced with pointer x-coordinate\n"
            "\n",
            bar->version);
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
}

static void init(struct bar* bar)
{
    pipe_init(bar);

    if (!(fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING)) {
        msg(bar, INNER_ERROR, "fcft version is lower then 2.4.0.");
    }

    bar->wl_display = wl_display_connect(NULL);
    if (bar->wl_display == NULL) {
        msg(bar, INNER_ERROR, "failed to connect to wayland display.");
    }
}

static void setup(struct bar* bar)
{
    bar->wl_registry = wl_display_get_registry(bar->wl_display);
    wl_registry_add_listener(bar->wl_registry, &wl_registry_listener, bar);

    if (wl_display_roundtrip(bar->wl_display) < 0) { // wait for wayland registry handlers
        msg(bar, INNER_ERROR, "failed to handle wayland display event queue.");
    } else if (bar->wl_compositor == NULL) {
        msg(bar, INNER_ERROR, "failed to get wayland compositor.");
    } else if (bar->wl_shm == NULL) {
        msg(bar, INNER_ERROR, "failed to get wayland shared memory.");
    } else if (bar->wp_fractional_scale_manager == NULL) {
        msg(bar, INNER_ERROR, "failed to get wayland fractional scale manager.");
    } else if (bar->wp_viewporter == NULL) {
        msg(bar, INNER_ERROR, "failed to get wayland viewporter.");
    } else if (bar->zwlr_layer_shell == NULL) {
        msg(bar, INNER_ERROR, "failed to get wayland layer shell.");
    } else if (!bar->wl_output_available) {
        msg(bar, INNER_ERROR, "failed to get any wayland output.");
    } else if (!bar->wl_seat_available) {
        msg(bar, INNER_ERROR, "failed to get any wayland seat.");
    }

    if (wl_display_roundtrip(bar->wl_display) < 0) { // wait for wayland output & seat handlers
        msg(bar, INNER_ERROR, "failed to handle wayland display event queue.");
    } else if (bar->wl_output_wrapper == NULL) {
        msg(bar, INNER_ERROR, "failed to get the wayland output %s.", bar->output);
    } else if (bar->wl_seat_wrapper == NULL) {
        msg(bar, INNER_ERROR, "failed to get the wayland seat %s.", bar->seat);
    }

    if (wl_display_roundtrip(bar->wl_display) < 0) { // wait for wayland output & seat handlers effects
        msg(bar, INNER_ERROR, "failed to handle wayland display event queue.");
    }
}

static void parse(struct bar* bar)
{
    for (int part_idx = 0; part_idx < PART_SIZE; part_idx++) {
        struct block *block, *tmp_block;
        wl_list_for_each_reverse_safe(block, tmp_block, &bar->part[part_idx], link)
        {
            wl_list_remove(&block->link);
            fcft_text_run_destroy(block->run);
            block->run = NULL;
            wl_list_insert(&bar->block, &block->link);
        }
    }

    const char* reader = bar->text.data;
    for (int part_idx = PART_LEFT; (void*)reader < bar->text.data + bar->text.size; part_idx++) {
        if (part_idx == PART_SIZE) {
            msg(bar, WARNING, "too many delimiters.");
            break;
        }

        struct block block = {
            .item = {
                { .value = "0", .last = &bar->part[part_idx] },
                { .value = "1", .last = &bar->part[part_idx] },
                { .value = "0", .last = &bar->part[part_idx] },
                { .value = NULL, .last = &bar->part[part_idx] },
                { .value = NULL, .last = &bar->part[part_idx] },
                { .value = NULL, .last = &bar->part[part_idx] },
                { .value = NULL, .last = &bar->part[part_idx] },
                { .value = NULL, .last = &bar->part[part_idx] },
                { .value = NULL, .last = &bar->part[part_idx] },
                { .value = NULL, .last = &bar->part[part_idx] },
            },
        };

        for (bool escape = false, delimiter = false;
            !delimiter && (void*)reader < bar->text.data + bar->text.size;
            escape = !escape, reader = reader + strlen(reader) + 1) {

            if (!escape) {
                struct block* insert_block = wl_container_of(bar->block.prev, insert_block, link);
                if (&insert_block->link == &bar->block) {
                    insert_block = calloc(1, sizeof(struct block));
                } else {
                    wl_list_remove(&insert_block->link);
                }
                *insert_block = block;
                insert_block->text = reader;
                wl_list_insert(&bar->part[part_idx], &insert_block->link);
            } else {
                if (reader[0] == 'D') {
                    delimiter = true;
                } else if (reader[0] == 'R') {
                    const char* tmp_color = block.item[ITEM_BG].value;
                    block.item[ITEM_BG].value = block.item[ITEM_FG].value;
                    block.item[ITEM_FG].value = tmp_color;
                    block.item[ITEM_BG].last = bar->part[part_idx].next;
                    block.item[ITEM_FG].last = bar->part[part_idx].next;
                } else {
                    int item_idx = ITEM_SIZE;
                    switch (reader[0]) {
                    case 'B':
                        item_idx = ITEM_BG;
                        break;
                    case 'F':
                        item_idx = ITEM_FG;
                        break;
                    case 'T':
                        item_idx = ITEM_FONT;
                        break;
                    case '1':
                        item_idx = ITEM_ACT1;
                        break;
                    case '2':
                        item_idx = ITEM_ACT2;
                        break;
                    case '3':
                        item_idx = ITEM_ACT3;
                        break;
                    case '4':
                        item_idx = ITEM_ACT4;
                        break;
                    case '5':
                        item_idx = ITEM_ACT5;
                        break;
                    case '6':
                        item_idx = ITEM_ACT6;
                        break;
                    case '7':
                        item_idx = ITEM_ACT7;
                        break;
                    }
                    if (item_idx == ITEM_SIZE) {
                        msg(bar, WARNING, "unkown escape characters: %s.\n", reader);
                        continue;
                    }
                    struct item* item = &block.item[item_idx];

                    if (reader[1] != '\0') {
                        item->value = reader + 1;
                        item->last = bar->part[part_idx].next;
                    } else {
                        if (item->last != &bar->part[part_idx]) {
                            const struct block* last_block = wl_container_of(item->last, last_block, link);
                            item->value = last_block->item[item_idx].value;
                            item->last = last_block->item[item_idx].last;
                        } else {
                            msg(bar, WARNING, "redundant restore operation: %s.", reader);
                        }
                    }
                }
            }
        }
    }

    bar->redraw = true;
}

static void randname(char* buf)
{
    struct timespec ts;
    clock_gettime(CLOCK_REALTIME, &ts);
    long r = ts.tv_nsec;
    for (int i = 0; i < 6; ++i) {
        buf[i] = 'A' + (r & 15) + (r & 16) * 2;
        r >>= 5;
    }
}

static int create_shm_file(void)
{
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

static int allocate_shm_file(size_t size)
{
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

static void wl_buffer_handle_release(void* data, struct wl_buffer* wl_buffer)
{
    struct wl_buffer_wrapper* bw = data;
    struct bar* bar = bw->bar;

    wl_list_remove(&bw->link);
    if (bar->wl_buffer_wrapper != NULL || (bw->height != bar->buf_height || bw->width != bar->buf_width)) {
        wl_buffer_wrapper_destroy(bw);
        free(bw);
    } else {
        bar->wl_buffer_wrapper = bw;
    }
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_handle_release,
};

static void prepare(struct bar* bar)
{
    if (bar->wl_buffer_wrapper != NULL) {
        struct wl_buffer_wrapper* bw = bar->wl_buffer_wrapper;
        if (bw->width != bar->buf_width || bw->height != bar->buf_height) {
            wl_buffer_wrapper_destroy(bw);
            free(bw);
            bar->wl_buffer_wrapper = NULL;
        }
    }

    if (bar->wl_buffer_wrapper == NULL) {
        int fd = allocate_shm_file(bar->buf_width * bar->buf_height * 4);
        if (fd == -1) {
            msg(bar, INNER_ERROR, "failed to allocate shared memory file.");
        }
        void* mmapped = mmap(NULL, bar->buf_width * bar->buf_height * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        if (mmapped == MAP_FAILED) {
            close(fd);
            msg(bar, INNER_ERROR, "failed to map shared memory file.");
        }
        struct wl_shm_pool* pool = wl_shm_create_pool(bar->wl_shm, fd, bar->buf_width * bar->buf_height * 4);
        struct wl_buffer* wl_buffer = wl_shm_pool_create_buffer(pool, 0, bar->buf_width, bar->buf_height, bar->buf_width * 4, WL_SHM_FORMAT_ARGB8888);
        wl_shm_pool_destroy(pool);
        close(fd);

        bar->wl_buffer_wrapper = calloc(1, sizeof(struct wl_buffer_wrapper));
        bar->wl_buffer_wrapper->width = bar->buf_width;
        bar->wl_buffer_wrapper->height = bar->buf_height;
        bar->wl_buffer_wrapper->mmapped = mmapped;
        bar->wl_buffer_wrapper->wl_buffer = wl_buffer;
        bar->wl_buffer_wrapper->bar = bar;
        wl_buffer_add_listener(wl_buffer, &wl_buffer_listener, bar->wl_buffer_wrapper);
    }
}

static void draw(struct bar* bar)
{
    prepare(bar);
    struct wl_buffer_wrapper* bw = bar->wl_buffer_wrapper;
    pixman_color_t* color = bar->color.data;
    struct fcft_font_wrapper* fw = bar->font.data;
    pixman_image_t* canvas = pixman_image_create_bits(PIXMAN_a8r8g8b8, bw->width, bw->height, bw->mmapped, bw->width * 4);

    pixman_box32_t bar_box = { 0, 0, bw->width, bw->height };
    pixman_image_fill_boxes(PIXMAN_OP_SRC, canvas, color, 1, &bar_box);

    pixman_image_t* bar_fg_image = pixman_image_create_solid_fill(color + 1);
    for (int part_idx = PART_LEFT; part_idx < PART_SIZE; part_idx++) {
        uint32_t part_width = 0;
        struct block* block;
        wl_list_for_each_reverse(block, &bar->part[part_idx], link)
        {
            if (block->text[0] == '\0') continue;

            int fw_idx = strtoul(block->item[ITEM_FONT].value, NULL, 10);
            if ((void*)(fw + fw_idx) >= bar->font.data + bar->font.size) {
                msg(bar, WARNING, "font index %ud is out of range. fallback to 0.", fw_idx);
                fw_idx = 0;
            }
            struct fcft_font* block_font = (fw + fw_idx)->font;
            block->y = (bw->height - block_font->height) / 2;
            block->height = block_font->height;
            block->base = (block_font->height + block_font->descent + block_font->ascent) / 2 - (block_font->descent > 0 ? block_font->descent : 0);

            bar->codepoint.size = 0;
            const char* reader = block->text;
            while (reader[0] != '\0') {
                uint32_t* codepoint = wl_array_add(&bar->codepoint, 4);
                if ((reader[0] & 0b10000000) == 0b00000000) {
                    *codepoint = reader[0];
                    reader += 1;
                } else if ((reader[0] & 0b11100000) == 0b11000000) {
                    *codepoint = ((reader[0] & 0b11111) << 6) | (reader[1] & 0b111111);
                    reader += 2;
                } else if ((reader[0] & 0b11110000) == 0b11100000) {
                    *codepoint = ((reader[0] & 0b1111) << 12) | ((reader[1] & 0b111111) << 6) | (reader[2] & 0b111111);
                    reader += 3;
                } else {
                    *codepoint = ((reader[0] & 0b111) << 18) | ((reader[1] & 0b111111) << 12) | ((reader[2] & 0b111111) << 6) | (reader[3] & 0b111111);
                    reader += 4;
                }
            }
            if (block->run != NULL) fcft_text_run_destroy(block->run);
            block->run = fcft_rasterize_text_run_utf32(block_font, bar->codepoint.size / 4, bar->codepoint.data, FCFT_SUBPIXEL_DEFAULT);
            block->width = 0;
            for (int i = 0; i < block->run->count; i++) {
                block->width += block->run->glyphs[i]->advance.x;
            }
            part_width += block->width;
        }
        if (part_width == 0) continue;
        uint32_t x = part_idx == PART_LEFT ? 0 : (part_idx == PART_RIGHT ? (bw->width - part_width) : ((bw->width - part_width) / 2));
        wl_list_for_each_reverse(block, &bar->part[part_idx], link)
        {
            block->x = x;
            if (block->width == 0) continue;

            int bg_idx = strtoul(block->item[ITEM_BG].value, NULL, 10);
            if ((void*)(color + bg_idx) >= bar->color.data + bar->color.size) {
                msg(bar, WARNING, "bg color index %ud is out of range. fallback to 0.", bg_idx);
                bg_idx = 0;
            }
            if (bg_idx != 0) {
                pixman_box32_t block_box = { block->x, block->y, block->x + block->width, block->y + block->height };
                pixman_image_fill_boxes(PIXMAN_OP_SRC, canvas, color + bg_idx, 1, &block_box);
            }
            for (int j = 0; j < block->run->count; j++) {
                const struct fcft_glyph* glyph = block->run->glyphs[j];
                if (glyph->is_color_glyph) {
                    pixman_image_composite32(PIXMAN_OP_OVER, glyph->pix, NULL, canvas, 0, 0, 0, 0, x + glyph->x, block->base + block->y - glyph->y, glyph->width, glyph->height);
                } else {
                    pixman_image_t* block_fg_image;
                    int fg_idx = strtol(block->item[ITEM_FG].value, NULL, 10);
                    if ((void*)(color + fg_idx) >= bar->color.data + bar->color.size) {
                        msg(bar, WARNING, "fg color index %ud is out of range. fallback to 1.", fg_idx);
                        fg_idx = 1;
                    }
                    if (fg_idx != 1) {
                        block_fg_image = pixman_image_create_solid_fill(color + fg_idx);
                    } else {
                        block_fg_image = pixman_image_ref(bar_fg_image);
                    }
                    pixman_image_composite32(PIXMAN_OP_OVER, block_fg_image, glyph->pix, canvas, 0, 0, 0, 0, x + glyph->x, block->base + block->y - glyph->y, glyph->width, glyph->height);
                    pixman_image_unref(block_fg_image);
                }
                x += glyph->advance.x;
            }
        }
    }

    pixman_image_unref(bar_fg_image);
    pixman_image_unref(canvas);

    wl_surface_set_buffer_scale(bar->wl_surface, 1);
    wl_surface_attach(bar->wl_surface, bw->wl_buffer, 0, 0);
    wl_surface_damage(bar->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(bar->wl_surface);
    wl_list_insert(&bar->wl_buffer_wrapper_busy, &bw->link);
    bar->wl_buffer_wrapper = NULL;

    bar->redraw = false;
}

static void loop(struct bar* bar)
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        msg(bar, INNER_ERROR, "failed to intercept signal.");
    }
    int signal_fd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (signal_fd < 0) {
        msg(bar, INNER_ERROR, "failed to create signal fd.");
    }

    int wl_display_fd = wl_display_get_fd(bar->wl_display);
    if (wl_display_fd < 0) {
        msg(bar, INNER_ERROR, "failed to get wayland display fd.");
    }

    int stdin_fd = STDIN_FILENO;

    struct pollfd pfds[] = {
        { .fd = signal_fd, .events = POLLIN },
        { .fd = wl_display_fd, .events = POLLIN },
        { .fd = stdin_fd, .events = POLLIN },
    };

    int x1f_count = 0;
    char* reader = NULL;
    while (true) {
        wl_display_flush(bar->wl_display);

        if (poll(pfds, 3, -1) < 0) {
            msg(bar, INNER_ERROR, "failed to wait for data using poll.");
        }

        if (pfds[0].revents & POLLIN) {
            msg(bar, NO_ERROR, "Interrupted by signal.");
        }

        if (pfds[1].revents & POLLIN) {
            if (wl_display_dispatch(bar->wl_display) < 0) {
                msg(bar, INNER_ERROR, "failed to handle wayland display event queue.");
            }
        }

        if (pfds[2].revents & POLLIN) {
            reader = wl_array_add(&bar->text_busy, 1);
            read(stdin_fd, reader, 1);
            if (reader[0] == '\x1f') {
                reader[0] = '\0';
                x1f_count++;
                if (x1f_count % 2 == 0 && reader[-1] == '\0') {
                    msg(bar, RUNTIME_ERROR, "empty is no allowed between a pair of \\x1f.");
                }
            } else if (reader[0] == '\n') {
                if (x1f_count % 2 == 0) {
                    reader[0] = '\0';

                    struct wl_array text_tmp = bar->text;
                    bar->text = bar->text_busy;
                    bar->text_busy = text_tmp;
                    bar->text_busy.size = 0;
                    x1f_count = 0;

                    parse(bar);
                } else {
                    msg(bar, RUNTIME_ERROR, "an odd number of \\x1f was found in stdin line.");
                }
            }
        } else if (pfds[2].revents & POLLHUP) {
            msg(bar, NO_ERROR, "STDIN EOF.");
        }

        if (bar->redraw) {
            draw(bar);
        }
    }
}

pixman_color_t strtocolor(const char* const color_str)
{
    uint32_t color_int = strtoul(color_str, NULL, 16);
    pixman_color_t color = {};
    if (strlen(color_str) > 6) {
        color.alpha = (color_int & 0xFF) * 0x0101;
        color_int >>= 8;
    } else {
        color.alpha = 0xFFFF;
    }
    color.blue = (color_int & 0xFF) * 0x0101;
    color_int >>= 8;
    color.green = (color_int & 0xFF) * 0x0101;
    color_int >>= 8;
    color.red = (color_int & 0xFF) * 0x0101;
    return color;
}

int main(int argc, char** argv)
{
    fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_ERROR);

    struct bar bar = {};
    bar.version = "2.0";
    char default_colors[] = "000000ff,ffffffff";
    bar.colors = default_colors;
    char default_fonts[] = "monospace";
    bar.fonts = default_fonts;
    bar.output = NULL;
    bar.seat = NULL;
    bar.bottom = false;
    bar.gap = 0;
    bar.throttle = 100;
    bar.repstr = "{}";

    char* env_colors = getenv("PBAR_COLORS");
    if (env_colors != NULL && strstr(env_colors, ",") != NULL) bar.colors = env_colors;
    char* env_fonts = getenv("PBAR_FONTS");
    if (env_fonts != NULL) bar.fonts = env_fonts;

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            if (++i < argc && argv[i][0] != '\0' && strstr(argv[i], ",") != NULL) bar.colors = argv[i];
        } else if (strcmp(argv[i], "-f") == 0) {
            if (++i < argc && argv[i][0] != '\0') bar.fonts = argv[i];
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i < argc && argv[i][0] != '\0') bar.output = argv[i];
        } else if (strcmp(argv[i], "-s") == 0) {
            if (++i < argc && argv[i][0] != '\0') bar.seat = argv[i];
        } else if (strcmp(argv[i], "-b") == 0) {
            bar.bottom = true;
        } else if (strcmp(argv[i], "-g") == 0) {
            if (++i < argc && argv[i][0] != '\0') bar.gap = strtoul(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "-i") == 0) {
            if (++i < argc && argv[i][0] != '\0') bar.throttle = strtoul(argv[i], NULL, 10);
        } else if (strcmp(argv[i], "-r") == 0) {
            if (++i < argc && argv[i][0] != '\0') bar.repstr = argv[i];
        }
    }

    wl_array_init(&bar.color);
    for (char *head = bar.colors, *reader = bar.colors;; reader++) {
        if (reader[0] != ',' && reader[0] != '\0') continue;
        bool end = false;
        if (reader[0] == '\0') {
            end = true;
        } else {
            reader[0] = '\0';
        }
        pixman_color_t* color = wl_array_add(&bar.color, sizeof(pixman_color_t));
        *color = strtocolor(head);
        if (end) {
            break;
        } else {
            head = reader + 1;
        }
    }
    wl_array_init(&bar.font);
    for (char *head = bar.fonts, *reader = bar.fonts;; reader++) {
        if (reader[0] != ',' && reader[0] != '\0') continue;
        bool end = false;
        if (reader[0] == '\0') {
            end = true;
        } else {
            reader[0] = '\0';
        }
        struct fcft_font_wrapper* fw = wl_array_add(&bar.font, sizeof(struct fcft_font_wrapper));
        fw->name = head;
        fw->font = fcft_from_name(1, (const char*[]) { head }, "dpi=96");
        if (fw->font->height > bar.height) {
            bar.height = fw->font->height;
        }
        if (end) {
            break;
        } else {
            head = reader + 1;
        }
    }
    wl_list_init(&bar.wl_buffer_wrapper_busy);
    wl_list_init(&bar.part[PART_LEFT]);
    wl_list_init(&bar.part[PART_CENTER]);
    wl_list_init(&bar.part[PART_RIGHT]);
    wl_list_init(&bar.block);
    wl_array_init(&bar.text);
    wl_array_init(&bar.text_busy);
    wl_array_init(&bar.codepoint);

    init(&bar);
    setup(&bar);
    loop(&bar);

    msg(&bar, NO_ERROR, NULL);
}
