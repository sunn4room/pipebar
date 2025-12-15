#include <errno.h>
#include <fcft/fcft.h>
#include <fcntl.h>
#include <linux/input-event-codes.h>
#include <pixman.h>
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
    PART_LEFT,
    PART_CENTER,
    PART_RIGHT,
    PART_SIZE,
};

struct block {
    struct entry* entry;
    uint32_t x, y, width, height, base;
    pixman_color_t *bg, *fg;
    struct fcft_font* font;
    struct fcft_text_run* run;
    struct wl_list link;
    struct bar* bar;
};

static void block_destroy(struct block* block)
{
    fcft_text_run_destroy(block->run);
    wl_list_remove(&block->link);
    free(block);
}

struct canvas {
    struct wl_buffer* wl_buffer;
    uint32_t width, height;
    void* mmapped;
    pixman_image_t* image;
    bool busy;
    struct wl_list link;
    struct bar* bar;
};

static void canvas_destroy(struct canvas* canvas)
{
    wl_list_remove(&canvas->link);
    pixman_image_unref(canvas->image);
    munmap(canvas->mmapped, canvas->width * canvas->height * 4);
    wl_buffer_destroy(canvas->wl_buffer);
    free(canvas);
}

struct bar {
    char name[16];
    struct wl_output* wl_output;
    uint32_t wl_output_name;
    struct wl_surface* wl_surface;
    struct wp_fractional_scale_v1* wp_fractional_scale;
    struct wp_viewport* wp_viewport;
    struct zwlr_layer_surface_v1* zwlr_layer_surface;
    uint32_t width, scale, canvas_width, canvas_height;
    struct wl_array font;
    struct wl_list part[PART_SIZE + 1];
    struct wl_list canvas;
    struct wl_list link;
    bool managed;
    bool redraw;
};

static void bar_destroy(struct bar* bar)
{
    struct fcft_font** font;
    wl_array_for_each(font, &bar->font)
    {
        fcft_destroy(*font);
    }
    wl_array_release(&bar->font);
    for (int part_idx = PART_LEFT; part_idx <= PART_SIZE; part_idx++) {
        struct block *block, *block_tmp;
        wl_list_for_each_safe(block, block_tmp, &bar->part[part_idx], link)
        {
            block_destroy(block);
        }
    }
    struct canvas *canvas, *canvas_tmp;
    wl_list_for_each_safe(canvas, canvas_tmp, &bar->canvas, link)
    {
        canvas_destroy(canvas);
    }
    if (bar->zwlr_layer_surface != NULL) zwlr_layer_surface_v1_destroy(bar->zwlr_layer_surface);
    if (bar->wp_viewport != NULL) wp_viewport_destroy(bar->wp_viewport);
    if (bar->wp_fractional_scale != NULL) wp_fractional_scale_v1_destroy(bar->wp_fractional_scale);
    if (bar->wl_surface != NULL) wl_surface_destroy(bar->wl_surface);
    wl_output_release(bar->wl_output);
    wl_list_remove(&bar->link);
    free(bar);
}

struct pointer {
    char name[16];
    struct wl_seat* wl_seat;
    uint32_t wl_seat_name;
    struct wl_pointer* wl_pointer;
    struct wl_surface* wl_surface;
    uint32_t x, y, time;
    struct wl_list link;
    bool managed;
};

static void pointer_destroy(struct pointer* pointer)
{
    if (pointer->wl_pointer != NULL) wl_pointer_release(pointer->wl_pointer);
    wl_seat_release(pointer->wl_seat);
    wl_list_remove(&pointer->link);
    free(pointer);
}

struct item {
    const char* value;
    struct wl_list* last;
};

enum {
    ITEM_BG,
    ITEM_FG,
    ITEM_FONT,
    ITEM_OUTPUT,
    ITEM_ACT1,
    ITEM_ACT2,
    ITEM_ACT3,
    ITEM_ACT4,
    ITEM_ACT5,
    ITEM_ACT6,
    ITEM_ACT7,
    ITEM_SIZE,
};

struct entry {
    struct item item[ITEM_SIZE];
    const char* text;
    struct wl_list link;
};

static void entry_destroy(struct entry* entry)
{
    wl_list_remove(&entry->link);
    free(entry);
}

struct pipebar {
    const char* version;
    bool debug;

    char* colors;
    struct wl_array color;
    char* fonts;
    struct wl_array font;
    char* outputs;
    struct wl_array output;
    char* seats;
    struct wl_array seat;
    bool bottom;
    uint32_t gap;
    uint32_t throttle;
    char* replace;

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

    uint32_t height;
    struct wl_list bar;
    struct wl_list pointer;

    struct wl_array text[2];
    struct wl_array codepoint;
    struct wl_list part[PART_SIZE + 1];
} pipebar;

static void pipebar_destroy()
{
    struct bar *bar, *bar_tmp;
    wl_list_for_each_safe(bar, bar_tmp, &pipebar.bar, link)
    {
        bar_destroy(bar);
    }
    struct pointer *pointer, *pointer_tmp;
    wl_list_for_each_safe(pointer, pointer_tmp, &pipebar.pointer, link)
    {
        pointer_destroy(pointer);
    }
    if (pipebar.zwlr_layer_shell != NULL) zwlr_layer_shell_v1_destroy(pipebar.zwlr_layer_shell);
    if (pipebar.wp_viewporter != NULL) wp_viewporter_destroy(pipebar.wp_viewporter);
    if (pipebar.wp_fractional_scale_manager != NULL) wp_fractional_scale_manager_v1_destroy(pipebar.wp_fractional_scale_manager);
    if (pipebar.wl_shm != NULL) wl_shm_release(pipebar.wl_shm);
    if (pipebar.wl_compositor != NULL) wl_compositor_destroy(pipebar.wl_compositor);
    if (pipebar.wl_registry != NULL) wl_registry_destroy(pipebar.wl_registry);
    if (pipebar.wl_display != NULL) wl_display_disconnect(pipebar.wl_display);
    wl_array_release(&pipebar.color);
    wl_array_release(&pipebar.font);
    wl_array_release(&pipebar.output);
    wl_array_release(&pipebar.seat);
    for (int i = 0; i < 2; i++) {
        wl_array_release(&pipebar.text[i]);
    }
    for (int part_idx = PART_LEFT; part_idx <= PART_SIZE; part_idx++) {
        struct entry *entry, *entry_tmp;
        wl_list_for_each_safe(entry, entry_tmp, &pipebar.part[part_idx], link)
        {
            entry_destroy(entry);
        }
    }
    fcft_fini();
}

enum {
    WARNING = -1,
    NO_ERROR = 0,
    INNER_ERROR = 1,
    RUNTIME_ERROR = 2,
};

static void msg(const int code, const char* restrict fmt, ...)
{
    if (fmt != NULL && fmt[0] != '\0') {
        va_list ap;
        va_start(ap, fmt);
        vfprintf(stderr, fmt, ap);
        va_end(ap);
        fputc('\n', stderr);
    }

    if (code < NO_ERROR) return;

    pipebar_destroy();
    exit(code);
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

static struct canvas* canvas_new(struct bar* bar)
{
    int fd = allocate_shm_file(bar->canvas_width * bar->canvas_height * 4);
    if (fd == -1) {
        msg(INNER_ERROR, "failed to allocate shared memory file.");
    }
    void* mmapped = mmap(NULL, bar->canvas_width * bar->canvas_height * 4, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (mmapped == MAP_FAILED) {
        close(fd);
        msg(INNER_ERROR, "failed to map shared memory file.");
    }
    pixman_image_t* image = pixman_image_create_bits(PIXMAN_a8r8g8b8, bar->canvas_width, bar->canvas_height, mmapped, bar->canvas_width * 4);
    struct wl_shm_pool* pool = wl_shm_create_pool(pipebar.wl_shm, fd, bar->canvas_width * bar->canvas_height * 4);
    struct wl_buffer* wl_buffer = wl_shm_pool_create_buffer(pool, 0, bar->canvas_width, bar->canvas_height, bar->canvas_width * 4, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    struct canvas* canvas = calloc(1, sizeof(struct canvas));
    canvas->width = bar->canvas_width;
    canvas->height = bar->canvas_height;
    canvas->mmapped = mmapped;
    canvas->image = image;
    canvas->wl_buffer = wl_buffer;
    canvas->bar = bar;
    wl_list_insert(&bar->canvas, &canvas->link);
    return canvas;
}

static struct block* block_new(struct bar* bar)
{
    struct block* block = calloc(1, sizeof(struct block));
    block->bar = bar;
    wl_list_insert(&bar->part[PART_SIZE], &block->link);
    return block;
}

static struct bar* bar_new(struct wl_output* wl_output, uint32_t name)
{
    struct bar* bar = calloc(1, sizeof(struct bar));
    bar->wl_output = wl_output;
    bar->wl_output_name = name;
    wl_array_init(&bar->font);
    for (int part_idx = PART_LEFT; part_idx <= PART_SIZE; part_idx++) {
        wl_list_init(&bar->part[part_idx]);
    }
    wl_list_init(&bar->canvas);
    wl_list_insert(&pipebar.bar, &bar->link);
    return bar;
}

static struct pointer* pointer_new(struct wl_seat* wl_seat, uint32_t name)
{
    struct pointer* pointer = calloc(1, sizeof(struct pointer));
    pointer->wl_seat = wl_seat;
    pointer->wl_seat_name = name;
    wl_list_insert(&pipebar.pointer, &pointer->link);
    return pointer;
}

static struct entry* entry_new()
{
    struct entry* entry = calloc(1, sizeof(struct entry));
    wl_list_insert(&pipebar.part[PART_SIZE], &entry->link);
    return entry;
}

static void pipebar_init()
{
    fcft_init(FCFT_LOG_COLORIZE_AUTO, false, FCFT_LOG_CLASS_ERROR);
    wl_array_init(&pipebar.color);
    wl_array_init(&pipebar.font);
    wl_array_init(&pipebar.output);
    wl_array_init(&pipebar.seat);
    wl_list_init(&pipebar.bar);
    wl_list_init(&pipebar.pointer);
    for (int i = 0; i < 2; i++) {
        wl_array_init(&pipebar.text[i]);
        wl_array_add(&pipebar.text[i], 256);
        pipebar.text[i].size = 0;
    }
    wl_array_init(&pipebar.codepoint);
    wl_array_add(&pipebar.codepoint, 256);
    pipebar.codepoint.size = 0;
    for (int part_idx = PART_LEFT; part_idx <= PART_SIZE; part_idx++) {
        wl_list_init(&pipebar.part[part_idx]);
    }
}

static void wl_buffer_handle_release(void* data, struct wl_buffer* wl_buffer)
{
    struct canvas* canvas = data;
    struct bar* bar = canvas->bar;

    if (canvas->height != bar->canvas_height || canvas->width != bar->canvas_width) {
        canvas_destroy(canvas);
    } else {
        wl_list_remove(&canvas->link);
        struct canvas* first_canvas = wl_container_of(bar->canvas.next, first_canvas, link);
        if (&first_canvas->link != &bar->canvas && !first_canvas->busy) {
            canvas_destroy(first_canvas);
        }
        canvas->busy = false;
        wl_list_insert(&bar->canvas, &canvas->link);
    }
}

static const struct wl_buffer_listener wl_buffer_listener = {
    .release = wl_buffer_handle_release,
};

static void wp_fractional_scale_handle_preferred_scale(void* data, struct wp_fractional_scale_v1* wp_fractional_scale_v1, uint32_t scale)
{
    struct bar* bar = data;
    bar->scale = scale;
    bar->canvas_width = bar->width * bar->scale / 120;

    struct fcft_font** font;
    wl_array_for_each(font, &bar->font)
    {
        fcft_destroy(*font);
    }
    bar->font.size = 0;

    char dpi[16];
    sprintf(dpi, "dpi=%u", 96 * bar->scale / 120);
    bar->canvas_height = 0;
    char** font_name;
    wl_array_for_each(font_name, &pipebar.font)
    {
        font = wl_array_add(&bar->font, sizeof(struct fcft_font*));
        *font = fcft_from_name(1, (const char*[]) { *font_name }, dpi);
        if ((*font)->height > bar->canvas_height) {
            bar->canvas_height = (*font)->height;
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
    wp_viewport_set_destination(bar->wp_viewport, bar->width, pipebar.height);
    bar->canvas_width = bar->width * bar->scale / 120;
    bar->redraw = true;
}

static void zwlr_layer_surface_handle_closed(void* data, struct zwlr_layer_surface_v1* zwlr_layer_surface_v1)
{
    struct bar* bar = data;
    bar_destroy(bar);
}

static const struct zwlr_layer_surface_v1_listener zwlr_layer_surface_listener = {
    .configure = zwlr_layer_surface_handle_configure,
    .closed = zwlr_layer_surface_handle_closed,
};

static void wl_output_handle_name(void* data, struct wl_output* wl_output, const char* name)
{
    struct bar* bar = data;
    strcpy(bar->name, name);
}

static void wl_output_handle_done(void* data, struct wl_output* wl_output)
{
    struct bar* bar = data;
    if (!bar->managed) {
        if (pipebar.outputs == NULL) {
            bar->managed = true;
        } else {
            char** name;
            wl_array_for_each(name, &pipebar.output)
            {
                if (strcmp(*name, bar->name) == 0) {
                    bar->managed = true;
                    break;
                }
            }
        }

        if (!bar->managed) {
            bar_destroy(bar);
            return;
        }

        bar->wl_surface = wl_compositor_create_surface(pipebar.wl_compositor);
        bar->wp_viewport = wp_viewporter_get_viewport(pipebar.wp_viewporter, bar->wl_surface);
        bar->wp_fractional_scale = wp_fractional_scale_manager_v1_get_fractional_scale(pipebar.wp_fractional_scale_manager, bar->wl_surface);
        wp_fractional_scale_v1_add_listener(bar->wp_fractional_scale, &wp_fractional_scale_listener, bar);
        bar->zwlr_layer_surface = zwlr_layer_shell_v1_get_layer_surface(pipebar.zwlr_layer_shell, bar->wl_surface, bar->wl_output, ZWLR_LAYER_SHELL_V1_LAYER_TOP, "statusbar");
        zwlr_layer_surface_v1_add_listener(bar->zwlr_layer_surface, &zwlr_layer_surface_listener, bar);
        zwlr_layer_surface_v1_set_anchor(bar->zwlr_layer_surface, ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT | ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT | (pipebar.bottom ? ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM : ZWLR_LAYER_SURFACE_V1_ANCHOR_TOP));
        zwlr_layer_surface_v1_set_margin(bar->zwlr_layer_surface, pipebar.gap, pipebar.gap, pipebar.gap, pipebar.gap);
        zwlr_layer_surface_v1_set_exclusive_zone(bar->zwlr_layer_surface, pipebar.height);
        zwlr_layer_surface_v1_set_size(bar->zwlr_layer_surface, 0, pipebar.height);
        wl_surface_commit(bar->wl_surface);
        return;
    }

    zwlr_layer_surface_v1_set_size(bar->zwlr_layer_surface, 0, pipebar.height);
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

static void wl_pointer_handle_enter(void* data, struct wl_pointer* wl_pointer, uint32_t serial, struct wl_surface* surface, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct pointer* pointer = data;
    pointer->wl_surface = surface;
    pointer->x = wl_fixed_to_double(surface_x);
    pointer->y = wl_fixed_to_double(surface_y);
}

static void wl_pointer_handle_leave(void* data, struct wl_pointer* wl_pointer, uint32_t serial, struct wl_surface* surface)
{
    struct pointer* pointer = data;
    pointer->wl_surface = NULL;
    pointer->x = UINT32_MAX;
    pointer->y = UINT32_MAX;
}

static void wl_pointer_handle_motion(void* data, struct wl_pointer* wl_pointer, uint32_t time, wl_fixed_t surface_x, wl_fixed_t surface_y)
{
    struct pointer* pointer = data;
    pointer->x = wl_fixed_to_double(surface_x);
    pointer->y = wl_fixed_to_double(surface_y);
}

static void action(struct pointer* pointer, int item_idx)
{
    struct bar* bar;
    wl_list_for_each(bar, &pipebar.bar, link)
    {
        if (bar->wl_surface == pointer->wl_surface) {
            if (!bar->redraw) {
                uint32_t x = pointer->x * bar->canvas_width / bar->width;
                uint32_t y = pointer->y * bar->canvas_height / pipebar.height;

                struct block* block;
                for (int part_idx = PART_LEFT; part_idx < PART_SIZE; part_idx++) {
                    wl_list_for_each_reverse(block, &bar->part[part_idx], link)
                    {
                        if (x < block->x) {
                            break;
                        } else if (x < block->x + block->width) {
                            const char* action = block->entry->item[item_idx].value;
                            if (action != NULL && y >= block->y && y < block->y + block->height) {
                                fprintf(stdout, "%s\n", action);
                            }
                            return;
                        }
                    }
                }
            }
            break;
        }
    }
}

static void wl_pointer_handle_button(void* data, struct wl_pointer* wl_pointer, uint32_t serial, uint32_t time, uint32_t button, uint32_t state)
{
    if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;

    struct pointer* pointer = data;

    if (time - pointer->time < pipebar.throttle) {
        return;
    } else {
        pointer->time = time;
    }

    if (button == BTN_LEFT) {
        action(pointer, ITEM_ACT1);
    } else if (button == BTN_MIDDLE) {
        action(pointer, ITEM_ACT2);
    } else if (button == BTN_RIGHT) {
        action(pointer, ITEM_ACT3);
    }
}

static void wl_pointer_handle_axis(void* data, struct wl_pointer* wl_pointer, uint32_t time, uint32_t axis, wl_fixed_t value)
{
    struct pointer* pointer = data;

    if (time - pointer->time < pipebar.throttle) {
        return;
    } else {
        pointer->time = time;
    }

    if (axis == 0 && value > 0) {
        action(pointer, ITEM_ACT4);
    } else if (axis == 0 && value < 0) {
        action(pointer, ITEM_ACT5);
    } else if (axis == 1 && value > 0) {
        action(pointer, ITEM_ACT6);
    } else if (axis == 1 && value < 0) {
        action(pointer, ITEM_ACT7);
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
    struct pointer* pointer = data;
    strcpy(pointer->name, name);
}

static void wl_seat_handle_capabilities(void* data, struct wl_seat* wl_seat, uint32_t capabilities)
{
    struct pointer* pointer = data;
    if (!pointer->managed) {
        if (pipebar.seats == NULL) {
            pointer->managed = true;
        } else {
            char** name;
            wl_array_for_each(name, &pipebar.seat)
            {
                if (strcmp(*name, pointer->name) == 0) {
                    pointer->managed = true;
                    break;
                }
            }
        }

        if (!pointer->managed) {
            pointer_destroy(pointer);
            return;
        }
    }

    bool have_pointer = capabilities & WL_SEAT_CAPABILITY_POINTER;
    if (have_pointer && pointer->wl_pointer == NULL) {
        pointer->wl_pointer = wl_seat_get_pointer(pointer->wl_seat);
        wl_pointer_add_listener(pointer->wl_pointer, &wl_pointer_listener, pointer);
    } else if (!have_pointer && pointer->wl_pointer != NULL) {
        wl_pointer_release(pointer->wl_pointer);
        pointer->wl_pointer = NULL;
        pointer->wl_surface = NULL;
        pointer->x = UINT32_MAX;
        pointer->y = UINT32_MAX;
    }
}

static const struct wl_seat_listener wl_seat_listener = {
    .name = wl_seat_handle_name,
    .capabilities = wl_seat_handle_capabilities,
};

static void wl_registry_handle_global(void* data, struct wl_registry* wl_registry, uint32_t name, const char* interface, uint32_t version)
{
    if (!strcmp(interface, wl_compositor_interface.name)) {
        pipebar.wl_compositor = wl_registry_bind(wl_registry, name, &wl_compositor_interface, 3);
        pipebar.wl_compositor_name = name;
    } else if (!strcmp(interface, wl_shm_interface.name)) {
        pipebar.wl_shm = wl_registry_bind(wl_registry, name, &wl_shm_interface, 2);
        pipebar.wl_shm_name = name;
    } else if (!strcmp(interface, wp_fractional_scale_manager_v1_interface.name)) {
        pipebar.wp_fractional_scale_manager = wl_registry_bind(wl_registry, name, &wp_fractional_scale_manager_v1_interface, 1);
        pipebar.zwlr_layer_shell_name = name;
    } else if (!strcmp(interface, wp_viewporter_interface.name)) {
        pipebar.wp_viewporter = wl_registry_bind(wl_registry, name, &wp_viewporter_interface, 1);
        pipebar.wp_viewporter_name = name;
    } else if (!strcmp(interface, zwlr_layer_shell_v1_interface.name)) {
        pipebar.zwlr_layer_shell = wl_registry_bind(wl_registry, name, &zwlr_layer_shell_v1_interface, 3);
        pipebar.zwlr_layer_shell_name = name;
    } else if (!strcmp(interface, wl_output_interface.name)) {
        struct bar* bar = bar_new(wl_registry_bind(wl_registry, name, &wl_output_interface, 4), name);
        wl_output_add_listener(bar->wl_output, &wl_output_listener, bar);
    } else if (!strcmp(interface, wl_seat_interface.name)) {
        struct pointer* pointer = pointer_new(wl_registry_bind(wl_registry, name, &wl_seat_interface, 5), name);
        wl_seat_add_listener(pointer->wl_seat, &wl_seat_listener, pointer);
    }
}

static void wl_registry_handle_global_remove(void* data, struct wl_registry* wl_registry, uint32_t name)
{
    if (name == pipebar.wl_compositor_name) {
        msg(INNER_ERROR, "Wayland compositor removed.");
    } else if (name == pipebar.wl_shm_name) {
        msg(INNER_ERROR, "Wayland shared memory removed.");
    } else if (name == pipebar.wp_fractional_scale_manager_name) {
        msg(INNER_ERROR, "Wayland fractional scale manager removed.");
    } else if (name == pipebar.wp_viewporter_name) {
        msg(INNER_ERROR, "Wayland viewporter removed.");
    } else if (name == pipebar.zwlr_layer_shell_name) {
        msg(INNER_ERROR, "Wayland layer shell removed.");
    } else {
        struct bar *bar, *bar_tmp;
        wl_list_for_each_safe(bar, bar_tmp, &pipebar.bar, link)
        {
            if (name == bar->wl_output_name) {
                bar_destroy(bar);
                return;
            }
        }
        struct pointer *pointer, *pointer_tmp;
        wl_list_for_each_safe(pointer, pointer_tmp, &pipebar.pointer, link)
        {
            if (name == pointer->wl_seat_name) {
                pointer_destroy(pointer);
                return;
            }
        }
    }
}

static const struct wl_registry_listener wl_registry_listener = {
    .global = wl_registry_handle_global,
    .global_remove = wl_registry_handle_global_remove,
};

static void set_pipe()
{
    struct stat stdin_stat;
    fstat(STDIN_FILENO, &stdin_stat);
    if (!S_ISFIFO(stdin_stat.st_mode)) {
        msg(NO_ERROR,
            "pipebar is a featherweight text-rendering wayland statusbar.\n"
            "It renders utf-8 sequence from STDIN line by line.\n"
            "It prints mouse pointer event actions to STDOUT.\n"
            "\n"
            "        version         %s\n"
            "        usage           producer | pipebar [options] | consumer\n"
            "\n"
            "Options are:\n"
            "        -c color,...    set colors list (000000ff,ffffffff)\n"
            "        -f font,...     set fonts list (monospace)\n"
            "        -o output,...   set wayland outputs list\n"
            "        -s seat,...     set wayland seats list\n"
            "        -b              place the bar at the bottom\n"
            "        -g gap          set margin gap (0)\n"
            "        -i interval     set pointer event throttle interval in ms (100)\n"
            "\n"
            "color can be: (support 0/1/2/3/4/6/8 hex numbers)\n"
            "        <empty>         -> 00000000\n"
            "        g               -> ggggggff\n"
            "        ga              -> ggggggaa\n"
            "        rgb             -> rrggbbff\n"
            "        rgba            -> rrggbbaa\n"
            "        rrggbb          -> rrggbbff\n"
            "        rrggbbaa        -> rrggbbaa\n"
            "\n"
            "font can be: (see 'man fcft_from_name' 'man fonts-conf')\n"
            "        name            font name\n"
            "        name:k=v        with single attribute\n"
            "        name:k=v:k=v    with multiple attributes\n"
            "\n"
            "output/seat can be: (see 'wayland-info')\n"
            "        name            output/seat name\n"
            "\n"
            "Sequence between a pair of '\\x1f' will be escaped instead of being rendered directly.\n"
            "Valid escape sequences are:\n"
            "        Bindex          set background color index (initially 0)\n"
            "        B               restore to last background color index\n"
            "        Findex          set foreground color index (initially 1)\n"
            "        F               restore to last foreground color index\n"
            "        Tindex          set font index (initially 0)\n"
            "        T               restore to last font index\n"
            "        Ooutput         set wayland output (initially NULL)\n"
            "        O               restore to last wayland output\n"
            "        1action         set left button click action (initially NULL)\n"
            "        1               restore to last left button click action\n"
            "        2action         set middle button click action (initially NULL)\n"
            "        2               restore to last middle button click action\n"
            "        3action         set right button click action (initially NULL)\n"
            "        3               restore to last right button click action\n"
            "        4action         set axis scroll down action (initially NULL)\n"
            "        4               restore to last axis scroll down action\n"
            "        5action         set axis scroll up action (initially NULL)\n"
            "        5               restore to last axis scroll up action\n"
            "        6action         set axis scroll left action (initially NULL)\n"
            "        6               restore to last axis scroll left action\n"
            "        7action         set axis scroll right action (initially NULL)\n"
            "        7               restore to last axis scroll right action\n"
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
            "\n",
            pipebar.version);
    }

    setvbuf(stdout, NULL, _IOLBF, 0);
}

static void parse()
{
    for (int part_idx = 0; part_idx < PART_SIZE; part_idx++) {
        struct entry *entry, *entry_tmp;
        wl_list_for_each_reverse_safe(entry, entry_tmp, &pipebar.part[part_idx], link)
        {
            wl_list_remove(&entry->link);
            wl_list_insert(&pipebar.part[PART_SIZE], &entry->link);
        }
    }

    const char* reader = pipebar.text[0].data;
    for (int part_idx = PART_LEFT; (void*)reader < pipebar.text[0].data + pipebar.text[0].size; part_idx++) {
        if (part_idx == PART_SIZE) {
            msg(WARNING, "too many delimiters.");
            break;
        }

        struct entry entry = {
            .item = {
                { .value = "0", .last = &pipebar.part[part_idx] },
                { .value = "1", .last = &pipebar.part[part_idx] },
                { .value = "0", .last = &pipebar.part[part_idx] },
                { .value = NULL, .last = &pipebar.part[part_idx] },
                { .value = NULL, .last = &pipebar.part[part_idx] },
                { .value = NULL, .last = &pipebar.part[part_idx] },
                { .value = NULL, .last = &pipebar.part[part_idx] },
                { .value = NULL, .last = &pipebar.part[part_idx] },
                { .value = NULL, .last = &pipebar.part[part_idx] },
                { .value = NULL, .last = &pipebar.part[part_idx] },
                { .value = NULL, .last = &pipebar.part[part_idx] },
            },
        };

        for (bool escape = false, delimiter = false;
            !delimiter && (void*)reader < pipebar.text[0].data + pipebar.text[0].size;
            escape = !escape, reader = reader + strlen(reader) + 1) {

            if (!escape) {
                struct entry* insert_entry = wl_container_of(pipebar.part[PART_SIZE].prev, insert_entry, link);
                if (&insert_entry->link == &pipebar.part[PART_SIZE]) {
                    insert_entry = entry_new();
                }
                wl_list_remove(&insert_entry->link);
                *insert_entry = entry;
                insert_entry->text = reader;
                wl_list_insert(&pipebar.part[part_idx], &insert_entry->link);
            } else {
                if (reader[0] == 'D') {
                    delimiter = true;
                } else if (reader[0] == 'R') {
                    const char* color_tmp = entry.item[ITEM_BG].value;
                    entry.item[ITEM_BG].value = entry.item[ITEM_FG].value;
                    entry.item[ITEM_FG].value = color_tmp;
                    entry.item[ITEM_BG].last = pipebar.part[part_idx].next;
                    entry.item[ITEM_FG].last = pipebar.part[part_idx].next;
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
                    case 'O':
                        item_idx = ITEM_OUTPUT;
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
                        msg(WARNING, "unkown escape characters: %s.\n", reader);
                        continue;
                    }
                    struct item* item = &entry.item[item_idx];

                    if (reader[1] != '\0') {
                        item->value = reader + 1;
                        item->last = pipebar.part[part_idx].next;
                    } else {
                        if (item->last != &pipebar.part[part_idx]) {
                            const struct entry* last_entry = wl_container_of(item->last, last_entry, link);
                            item->value = last_entry->item[item_idx].value;
                            item->last = last_entry->item[item_idx].last;
                        } else {
                            msg(WARNING, "redundant restore operation: %s.", reader);
                        }
                    }
                }
            }
        }
    }

    struct bar* bar;
    wl_list_for_each(bar, &pipebar.bar, link)
    {
        bar->redraw = true;
    }
}

static struct canvas* bar_get_canvas(struct bar* bar)
{
    struct canvas* canvas = wl_container_of(bar->canvas.next, canvas, link);
    if (&canvas->link == &bar->canvas || canvas->busy) {
        canvas = NULL;
    } else if (canvas->width != bar->canvas_width || canvas->height != bar->canvas_height) {
        canvas_destroy(canvas);
        canvas = NULL;
    }
    if (canvas == NULL) {
        canvas = canvas_new(bar);
        wl_buffer_add_listener(canvas->wl_buffer, &wl_buffer_listener, canvas);
    }
    return canvas;
}

static void draw(struct bar* bar)
{
    struct canvas* canvas = bar_get_canvas(bar);

    for (int part_idx = 0; part_idx < PART_SIZE; part_idx++) {
        struct block *block, *block_tmp;
        wl_list_for_each_reverse_safe(block, block_tmp, &bar->part[part_idx], link)
        {
            wl_list_remove(&block->link);
            fcft_text_run_destroy(block->run);
            block->run = NULL;
            wl_list_insert(&bar->part[PART_SIZE], &block->link);
        }
    }

    struct fcft_font** font = bar->font.data;
    pixman_color_t* color = pipebar.color.data;

    pixman_box32_t bar_box = { 0, 0, canvas->width, canvas->height };
    pixman_image_fill_boxes(PIXMAN_OP_SRC, canvas->image, color, 1, &bar_box);
    pixman_image_t* bar_fg_image = pixman_image_create_solid_fill(color + 1);
    for (int part_idx = PART_LEFT; part_idx < PART_SIZE; part_idx++) {
        uint32_t part_width = 0;
        struct entry* entry;
        struct block* block;
        wl_list_for_each_reverse(entry, &pipebar.part[part_idx], link)
        {
            if (entry->text[0] == '\0') continue;
            if (entry->item[ITEM_OUTPUT].value != NULL) {
                struct entry* each = entry;
                do {
                    if (strcmp(each->item[ITEM_OUTPUT].value, bar->name) == 0) break;
                    each = wl_container_of(each->item[ITEM_OUTPUT].last, each, link);
                } while (each->item[ITEM_OUTPUT].value != NULL);
                if (each->item[ITEM_OUTPUT].value == NULL) continue;
            }

            block = wl_container_of(bar->part[PART_SIZE].prev, block, link);
            if (&block->link == &bar->part[PART_SIZE]) {
                block = block_new(bar);
            }

            wl_list_remove(&block->link);
            wl_list_insert(&bar->part[part_idx], &block->link);
            block->entry = entry;

            int bg_idx = strtoul(entry->item[ITEM_BG].value, NULL, 10);
            if ((void*)(color + bg_idx) >= pipebar.color.data + pipebar.color.size) {
                msg(WARNING, "bg color index %ud is out of range. fallback to 0.", bg_idx);
                bg_idx = 0;
            }
            block->bg = color + bg_idx;

            int fg_idx = strtoul(entry->item[ITEM_FG].value, NULL, 10);
            if ((void*)(color + fg_idx) >= pipebar.color.data + pipebar.color.size) {
                msg(WARNING, "fg color index %ud is out of range. fallback to 1.", fg_idx);
                fg_idx = 1;
            }
            block->fg = color + fg_idx;

            int font_idx = strtoul(entry->item[ITEM_FONT].value, NULL, 10);
            if ((void*)(font + font_idx) >= bar->font.data + bar->font.size) {
                msg(WARNING, "font index %ud is out of range. fallback to 0.", font_idx);
                font_idx = 0;
            }
            block->font = font[font_idx];

            block->y = (canvas->height - block->font->height) / 2;
            block->height = block->font->height;
            block->base = (block->font->height + block->font->descent + block->font->ascent) / 2 - (block->font->descent > 0 ? block->font->descent : 0);

            pipebar.codepoint.size = 0;
            const char* reader = entry->text;
            while (reader[0] != '\0') {
                uint32_t* codepoint = wl_array_add(&pipebar.codepoint, 4);
                if ((reader[0] & 0b10000000) == 0b00000000) {
                    *codepoint = reader[0];
                    reader += 1;
                } else if ((reader[0] & 0b11100000) == 0b11000000) {
                    if ((reader[1] & 0b11000000) != 0b10000000) {
                        msg(RUNTIME_ERROR, "invalid utf-8 character sequence.");
                    }
                    *codepoint = ((reader[0] & 0b11111) << 6) | (reader[1] & 0b111111);
                    reader += 2;
                } else if ((reader[0] & 0b11110000) == 0b11100000) {
                    if ((reader[1] & 0b11000000) != 0b10000000 || (reader[2] & 0b11000000) != 0b10000000) {
                        msg(RUNTIME_ERROR, "invalid utf-8 character sequence.");
                    }
                    *codepoint = ((reader[0] & 0b1111) << 12) | ((reader[1] & 0b111111) << 6) | (reader[2] & 0b111111);
                    reader += 3;
                } else if ((reader[0] & 0b11111000) == 0b11110000) {
                    if ((reader[1] & 0b11000000) != 0b10000000 || (reader[2] & 0b11000000) != 0b10000000 || (reader[3] & 0b11000000) != 0b10000000) {
                        msg(RUNTIME_ERROR, "invalid utf-8 character sequence.");
                    }
                    *codepoint = ((reader[0] & 0b111) << 18) | ((reader[1] & 0b111111) << 12) | ((reader[2] & 0b111111) << 6) | (reader[3] & 0b111111);
                    reader += 4;
                } else {
                    msg(RUNTIME_ERROR, "invalid utf-8 character sequence.");
                }
            }
            block->run = fcft_rasterize_text_run_utf32(block->font, pipebar.codepoint.size / 4, pipebar.codepoint.data, FCFT_SUBPIXEL_DEFAULT);
            block->width = 0;
            for (int i = 0; i < block->run->count; i++) {
                block->width += block->run->glyphs[i]->advance.x;
            }
            part_width += block->width;
        }
        if (part_width == 0) continue;
        uint32_t x = part_idx == PART_LEFT ? 0 : (part_idx == PART_RIGHT ? (canvas->width - part_width) : ((canvas->width - part_width) / 2));
        wl_list_for_each_reverse(block, &bar->part[part_idx], link)
        {
            block->x = x;

            if (block->bg != color) {
                pixman_box32_t block_box = { block->x, block->y, block->x + block->width, block->y + block->height };
                pixman_image_fill_boxes(PIXMAN_OP_SRC, canvas->image, block->bg, 1, &block_box);
            }

            for (int j = 0; j < block->run->count; j++) {
                const struct fcft_glyph* glyph = block->run->glyphs[j];
                if (glyph->is_color_glyph) {
                    pixman_image_composite32(PIXMAN_OP_OVER, glyph->pix, NULL, canvas->image, 0, 0, 0, 0, x + glyph->x, block->base + block->y - glyph->y, glyph->width, glyph->height);
                } else {
                    pixman_image_t* block_fg_image;
                    if (block->fg != color + 1) {
                        block_fg_image = pixman_image_create_solid_fill(block->fg);
                    } else {
                        block_fg_image = pixman_image_ref(bar_fg_image);
                    }
                    pixman_image_composite32(PIXMAN_OP_OVER, block_fg_image, glyph->pix, canvas->image, 0, 0, 0, 0, x + glyph->x, block->base + block->y - glyph->y, glyph->width, glyph->height);
                    pixman_image_unref(block_fg_image);
                }
                x += glyph->advance.x;
            }
        }
    }

    pixman_image_unref(bar_fg_image);

    wl_surface_set_buffer_scale(bar->wl_surface, 1);
    wl_surface_attach(bar->wl_surface, canvas->wl_buffer, 0, 0);
    wl_surface_damage(bar->wl_surface, 0, 0, INT32_MAX, INT32_MAX);
    wl_surface_commit(bar->wl_surface);
    canvas->busy = true;

    bar->redraw = false;
}

pixman_color_t strtocolor(const char* const str)
{
    pixman_color_t color = {};
    char color_builder[9] = {};
    switch (strlen(str)) {
    case 0:
        return color;
    case 1:
        sprintf(color_builder, "%c%c%c%c%c%cff", str[0], str[0], str[0], str[0], str[0], str[0]);
        break;
    case 2:
        sprintf(color_builder, "%c%c%c%c%c%c%c%c", str[0], str[0], str[0], str[0], str[0], str[0], str[1], str[1]);
        break;
    case 3:
        sprintf(color_builder, "%c%c%c%c%c%cff", str[0], str[0], str[1], str[1], str[2], str[2]);
        break;
    case 4:
        sprintf(color_builder, "%c%c%c%c%c%c%c%c", str[0], str[0], str[1], str[1], str[2], str[2], str[3], str[3]);
        break;
    case 6:
        sprintf(color_builder, "%sff", str);
        break;
    case 8:
        sprintf(color_builder, "%s", str);
        break;
    }
    if (color_builder[0] != '\0') {
        char* endptr;
        uint32_t color_int = strtoul(color_builder, &endptr, 16);
        if (*endptr == '\0') {
            color.alpha = (color_int & 0xFF) * 0x0101;
            color_int >>= 8;
            color.blue = (color_int & 0xFF) * 0x0101;
            color_int >>= 8;
            color.green = (color_int & 0xFF) * 0x0101;
            color_int >>= 8;
            color.red = (color_int & 0xFF) * 0x0101;
            return color;
        }
    }
    msg(RUNTIME_ERROR, "option -c got a invalid color: %s.", str);
    return color;
}

char default_colors[] = "000000ff,ffffffff";
char default_fonts[] = "monospace";

static void init(int argc, char** argv)
{
    pipebar_init();

    pipebar.version = "3.3";

    pipebar.colors = default_colors;
    pipebar.fonts = default_fonts;
    pipebar.outputs = NULL;
    pipebar.seats = NULL;
    pipebar.bottom = false;
    pipebar.gap = 0;
    pipebar.throttle = 100;
    pipebar.replace = "{}";

    for (int i = 1; i < argc; i++) {
        if (strcmp(argv[i], "-c") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                pipebar.colors = argv[i];
            } else {
                msg(RUNTIME_ERROR, "option %s requires an argument.", argv[i - 1]);
            }
        } else if (strcmp(argv[i], "-f") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                pipebar.fonts = argv[i];
            } else {
                msg(RUNTIME_ERROR, "option %s requires an argument.", argv[i - 1]);
            }
        } else if (strcmp(argv[i], "-o") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                pipebar.outputs = argv[i];
            } else {
                msg(RUNTIME_ERROR, "option %s requires an argument.", argv[i - 1]);
            }
        } else if (strcmp(argv[i], "-s") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                pipebar.seats = argv[i];
            } else {
                msg(RUNTIME_ERROR, "option %s requires an argument.", argv[i - 1]);
            }
        } else if (strcmp(argv[i], "-b") == 0) {
            pipebar.bottom = true;
        } else if (strcmp(argv[i], "-g") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                char* endptr;
                pipebar.gap = strtoul(argv[i], &endptr, 10);
                if (*endptr != '\0') {
                    msg(RUNTIME_ERROR, "option %s got a invalid argument: %s.", argv[i - 1], argv[i]);
                }
            } else {
                msg(RUNTIME_ERROR, "option %s requires an argument.", argv[i - 1]);
            }
        } else if (strcmp(argv[i], "-i") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                char* endptr;
                pipebar.throttle = strtoul(argv[i], &endptr, 10);
                if (*endptr != '\0') {
                    msg(RUNTIME_ERROR, "option %s got a invalid argument: %s.", argv[i - 1], argv[i]);
                }
            } else {
                msg(RUNTIME_ERROR, "option %s requires an argument.", argv[i - 1]);
            }
        } else if (strcmp(argv[i], "-r") == 0) {
            if (++i < argc && argv[i][0] != '\0') {
                pipebar.replace = argv[i];
            } else {
                msg(RUNTIME_ERROR, "option %s requires an argument.", argv[i - 1]);
            }
        }
    }

    for (char *head = pipebar.colors, *reader = pipebar.colors;; reader++) {
        if (reader[0] != ',' && reader[0] != '\0') continue;
        bool end = false;
        if (reader[0] == '\0') {
            end = true;
        } else {
            reader[0] = '\0';
        }
        pixman_color_t* color = wl_array_add(&pipebar.color, sizeof(pixman_color_t));
        *color = strtocolor(head);
        if (end) {
            break;
        } else {
            head = reader + 1;
        }
    }
    if (pipebar.color.size == sizeof(pixman_color_t)) {
        msg(RUNTIME_ERROR, "option -c need at least two color.");
    }

    for (char *head = pipebar.fonts, *reader = pipebar.fonts;; reader++) {
        if (reader[0] != ',' && reader[0] != '\0') continue;
        bool end = false;
        if (reader[0] == '\0') {
            end = true;
        } else {
            reader[0] = '\0';
        }
        char** name = wl_array_add(&pipebar.font, sizeof(char*));
        *name = head;
        struct fcft_font* font = fcft_from_name(1, (const char*[]) { *name }, "dpi=96");
        if (font->height > pipebar.height) {
            pipebar.height = font->height;
        }
        fcft_destroy(font);
        if (end) {
            break;
        } else {
            head = reader + 1;
        }
    }

    if (pipebar.outputs != NULL) {
        for (char *head = pipebar.outputs, *reader = pipebar.outputs;; reader++) {
            if (reader[0] != ',' && reader[0] != '\0') continue;
            char** name = wl_array_add(&pipebar.output, sizeof(char*));
            *name = head;
            if (reader[0] == '\0') {
                break;
            } else {
                reader[0] = '\0';
                head = reader + 1;
            }
        }
    }

    if (pipebar.seats != NULL) {
        for (char *head = pipebar.seats, *reader = pipebar.seats;; reader++) {
            if (reader[0] != ',' && reader[0] != '\0') continue;
            char** name = wl_array_add(&pipebar.seat, sizeof(char*));
            *name = head;
            if (reader[0] == '\0') {
                break;
            } else {
                reader[0] = '\0';
                head = reader + 1;
            }
        }
    }
}

static void setup()
{
    set_pipe();

    if (!(fcft_capabilities() & FCFT_CAPABILITY_TEXT_RUN_SHAPING)) {
        msg(INNER_ERROR, "fcft version is lower then 2.4.0.");
    }

    pipebar.wl_display = wl_display_connect(NULL);
    if (pipebar.wl_display == NULL) {
        msg(INNER_ERROR, "failed to connect to wayland display.");
    }

    pipebar.wl_registry = wl_display_get_registry(pipebar.wl_display);
    wl_registry_add_listener(pipebar.wl_registry, &wl_registry_listener, NULL);

    if (wl_display_roundtrip(pipebar.wl_display) < 0) {
        msg(INNER_ERROR, "failed to handle wayland display event queue.");
    } else if (pipebar.wl_compositor == NULL) {
        msg(INNER_ERROR, "failed to get wayland compositor.");
    } else if (pipebar.wl_shm == NULL) {
        msg(INNER_ERROR, "failed to get wayland shared memory.");
    } else if (pipebar.wp_fractional_scale_manager == NULL) {
        msg(INNER_ERROR, "failed to get wayland fractional scale manager.");
    } else if (pipebar.wp_viewporter == NULL) {
        msg(INNER_ERROR, "failed to get wayland viewporter.");
    } else if (pipebar.zwlr_layer_shell == NULL) {
        msg(INNER_ERROR, "failed to get wayland layer shell.");
    }
}

static void loop()
{
    sigset_t mask;
    sigemptyset(&mask);
    sigaddset(&mask, SIGTERM);
    sigaddset(&mask, SIGINT);
    if (sigprocmask(SIG_BLOCK, &mask, NULL) == -1) {
        msg(INNER_ERROR, "failed to intercept signal.");
    }
    int signal_fd = signalfd(-1, &mask, SFD_NONBLOCK);
    if (signal_fd < 0) {
        msg(INNER_ERROR, "failed to create signal fd.");
    }

    int wl_display_fd = wl_display_get_fd(pipebar.wl_display);
    if (wl_display_fd < 0) {
        msg(INNER_ERROR, "failed to get wayland display fd.");
    }

    int stdin_fd = STDIN_FILENO;

    struct pollfd pfds[] = {
        { .fd = signal_fd, .events = POLLIN },
        { .fd = stdin_fd, .events = POLLIN },
        { .fd = wl_display_fd, .events = POLLIN },
    };

    int x1f_count = 0;
    char* reader = NULL;
    while (true) {
        wl_display_flush(pipebar.wl_display);

        if (poll(pfds, 3, -1) < 0) {
            msg(INNER_ERROR, "failed to wait for data using poll.");
        }

        if (pfds[0].revents & POLLIN) {
            msg(NO_ERROR, "Interrupted by signal.");
        }

        if (pfds[1].revents & POLLHUP) {
            msg(NO_ERROR, "STDIN EOF.");
        }

        if (pfds[1].revents & POLLIN) {
            reader = wl_array_add(&pipebar.text[1], 1);
            read(stdin_fd, reader, 1);
            if (reader[0] == '\x1f') {
                reader[0] = '\0';
                x1f_count++;
                if (x1f_count % 2 == 0 && reader[-1] == '\0') {
                    msg(RUNTIME_ERROR, "empty between a pair of \\x1f.");
                }
            } else if (reader[0] == '\n') {
                if (x1f_count % 2 == 0) {
                    reader[0] = '\0';

                    struct wl_array text_tmp = pipebar.text[0];
                    pipebar.text[0] = pipebar.text[1];
                    pipebar.text[1] = text_tmp;
                    pipebar.text[1].size = 0;
                    x1f_count = 0;

                    parse();
                } else {
                    msg(RUNTIME_ERROR, "got an odd number of '\\x1f'.");
                }
            }
        }

        if (pfds[2].revents & POLLIN) {
            if (wl_display_dispatch(pipebar.wl_display) < 0) {
                msg(INNER_ERROR, "failed to handle wayland display event queue.");
            }
        }

        struct bar* bar;
        wl_list_for_each(bar, &pipebar.bar, link)
        {
            if (bar->redraw && bar->width != 0 && bar->scale != 0) {
                draw(bar);
            }
        }
    }
}

int main(int argc, char** argv)
{
    init(argc, argv);
    setup();
    loop();

    return NO_ERROR;
}
