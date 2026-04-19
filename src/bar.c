#include "bar.h"
#include "registry.h"
#include "workspace.h"
#include "battery.h"
#include "volume.h"
#include "clock.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <signal.h>
#include <poll.h>
#include <time.h>
#include <fcntl.h>
#include <errno.h>
#include <math.h>
#include <sys/mman.h>
#include <cairo/cairo.h>

#define BAR_HEIGHT 28

static int create_shm_file(size_t size) {
    int fd = -1;
    char name[] = "/simpbar-XXXXXX";
    fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        shm_unlink(name);
    } else {
        char path[] = "/tmp/simpbar-XXXXXX";
        fd = mkstemp(path);
        if (fd >= 0) unlink(path);
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) {
        close(fd);
        return -1;
    }
    return fd;
}

struct buffer {
    struct wl_buffer *wl_buf;
    void *data;
    size_t size;
    int fd;
    bool busy;
    cairo_surface_t *cairo_surface;
    int width, height, stride;
};

static void buffer_release(void *data, struct wl_buffer *wl_buf) {
    (void)wl_buf;
    struct buffer *buf = data;
    buf->busy = false;
}

static const struct wl_buffer_listener buffer_listener = {
    .release = buffer_release,
};

static struct buffer *buffer_create(struct bar *bar) {
    int pw = bar->width * bar->scale;
    int ph = bar->height * bar->scale;
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, pw);
    size_t size = stride * ph;

    int fd = create_shm_file(size);
    if (fd < 0) return NULL;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) {
        close(fd);
        return NULL;
    }

    struct wl_shm_pool *pool = wl_shm_create_pool(bar->shm, fd, size);
    struct wl_buffer *wl_buf = wl_shm_pool_create_buffer(
        pool, 0, pw, ph, stride, WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);

    struct buffer *buf = calloc(1, sizeof(*buf));
    buf->wl_buf = wl_buf;
    buf->data = data;
    buf->size = size;
    buf->fd = fd;
    buf->busy = false;
    buf->width = pw;
    buf->height = ph;
    buf->stride = stride;
    buf->cairo_surface = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32, pw, ph, stride);
    cairo_surface_set_device_scale(buf->cairo_surface, bar->scale, bar->scale);

    wl_buffer_add_listener(wl_buf, &buffer_listener, buf);
    return buf;
}

static void buffer_destroy(struct buffer *buf) {
    if (!buf) return;
    if (buf->cairo_surface) cairo_surface_destroy(buf->cairo_surface);
    if (buf->wl_buf) wl_buffer_destroy(buf->wl_buf);
    if (buf->data) munmap(buf->data, buf->size);
    if (buf->fd >= 0) close(buf->fd);
    free(buf);
}

static void render_content(struct bar *bar, cairo_t *cr) {
    int w = bar->width;
    int h = bar->height;
    double cy = h / 2.0;
    double font_sz = 12;
    double section_gap = 18;

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.07, 0.07, 0.10, 0.92);
    cairo_paint(cr);

    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    cairo_select_font_face(cr, "sans-serif",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);
    cairo_set_font_size(cr, font_sz);

    cairo_font_extents_t fe;
    cairo_font_extents(cr, &fe);
    double text_y = cy + (fe.ascent - fe.descent) / 2.0;

    cairo_text_extents_t ext;
    double rx = w - 14;
    char buf[128];

    // Clock (rightmost)
    snprintf(buf, sizeof(buf), "%s  %s", bar->clock->date_str, bar->clock->time_str);
    cairo_text_extents(cr, buf, &ext);
    rx -= ext.width;
    cairo_set_source_rgb(cr, 0.88, 0.88, 0.90);
    cairo_move_to(cr, rx, text_y);
    cairo_show_text(cr, buf);
    rx -= section_gap;

    // Battery icon + text
    int pct = bar->battery->percentage;
    snprintf(buf, sizeof(buf), "%d%%", pct);
    cairo_text_extents(cr, buf, &ext);
    rx -= ext.width;
    double g = pct / 100.0;
    cairo_set_source_rgb(cr, 0.3 + (1.0-g)*0.6, 0.3 + g*0.5, 0.3);
    cairo_move_to(cr, rx, text_y);
    cairo_show_text(cr, buf);
    rx -= 5;

    {
        double bw = 16, bh = 8;
        double ix = rx - bw - 2, iy = cy;
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
        cairo_rectangle(cr, ix, iy - bh/2, bw, bh);
        cairo_stroke(cr);
        cairo_rectangle(cr, ix + bw, iy - 1.5, 2, 3);
        cairo_fill(cr);
        double fill_w = (bw - 2) * g;
        cairo_set_source_rgb(cr, 0.3 + (1.0-g)*0.6, 0.3 + g*0.5, 0.3);
        cairo_rectangle(cr, ix + 1, iy - bh/2 + 1, fill_w, bh - 2);
        cairo_fill(cr);
        if (bar->battery->state == BATTERY_CHARGING) {
            cairo_set_source_rgb(cr, 1.0, 1.0, 0.4);
            cairo_set_line_width(cr, 1.2);
            cairo_move_to(cr, ix + bw/2 + 1.5, iy - 3);
            cairo_line_to(cr, ix + bw/2 - 0.5, iy);
            cairo_line_to(cr, ix + bw/2 + 1.5, iy);
            cairo_line_to(cr, ix + bw/2 - 0.5, iy + 3);
            cairo_stroke(cr);
        }
        rx = ix - section_gap;
    }

    // Volume icon + text
    int vol = bar->volume->level;
    snprintf(buf, sizeof(buf), "%d%%", vol);
    cairo_text_extents(cr, buf, &ext);
    rx -= ext.width;
    cairo_set_source_rgb(cr, 0.88, 0.88, 0.90);
    cairo_move_to(cr, rx, text_y);
    cairo_show_text(cr, buf);
    rx -= 5;

    {
        double ix = rx - 12, iy = cy;
        cairo_set_source_rgb(cr, 0.88, 0.88, 0.90);
        cairo_rectangle(cr, ix, iy - 2.5, 3.5, 5);
        cairo_fill(cr);
        cairo_move_to(cr, ix + 3.5, iy - 2.5);
        cairo_line_to(cr, ix + 7.5, iy - 5);
        cairo_line_to(cr, ix + 7.5, iy + 5);
        cairo_line_to(cr, ix + 3.5, iy + 2.5);
        cairo_close_path(cr);
        cairo_fill(cr);
        if (bar->volume->muted) {
            cairo_set_line_width(cr, 1.2);
            cairo_move_to(cr, ix + 9, iy - 3);
            cairo_line_to(cr, ix + 14, iy + 3);
            cairo_move_to(cr, ix + 14, iy - 3);
            cairo_line_to(cr, ix + 9, iy + 3);
            cairo_stroke(cr);
        } else {
            cairo_set_line_width(cr, 1.0);
            if (vol > 33) {
                cairo_arc(cr, ix + 8.5, iy, 3.5, -M_PI/4, M_PI/4);
                cairo_stroke(cr);
            }
            if (vol > 66) {
                cairo_arc(cr, ix + 8.5, iy, 6, -M_PI/4, M_PI/4);
                cairo_stroke(cr);
            }
        }
        rx = ix - section_gap;
    }

    // Workspace indicators — neutral colors
    for (int i = bar->workspace_mgr->workspace_count - 1; i >= 0 && i < 8; i--) {
        if (bar->workspace_mgr->workspaces[i].state == WORKSPACE_ACTIVE) {
            cairo_set_source_rgb(cr, 0.80, 0.80, 0.82);
            double rw = 24, rh = 4, radius = 2;
            rx -= rw;
            double ry = cy - rh / 2.0;
            cairo_new_sub_path(cr);
            cairo_arc(cr, rx + rw - radius, ry + radius, radius, -M_PI/2, 0);
            cairo_arc(cr, rx + rw - radius, ry + rh - radius, radius, 0, M_PI/2);
            cairo_arc(cr, rx + radius, ry + rh - radius, radius, M_PI/2, M_PI);
            cairo_arc(cr, rx + radius, ry + radius, radius, M_PI, 3*M_PI/2);
            cairo_close_path(cr);
            cairo_fill(cr);
            rx -= 10;
        } else {
            cairo_set_source_rgb(cr, 0.40, 0.40, 0.45);
            rx -= 7;
            cairo_arc(cr, rx + 3, cy, 3, 0, 2 * M_PI);
            cairo_fill(cr);
            rx -= 10;
        }
    }
}

static struct buffer *buffers[2] = {NULL, NULL};

static struct buffer *get_buffer(struct bar *bar) {
    int pw = bar->width * bar->scale;
    int ph = bar->height * bar->scale;
    for (int i = 0; i < 2; i++) {
        if (buffers[i] && !buffers[i]->busy &&
            buffers[i]->width == pw && buffers[i]->height == ph)
            return buffers[i];
    }
    for (int i = 0; i < 2; i++) {
        if (!buffers[i]) {
            buffers[i] = buffer_create(bar);
            return buffers[i];
        }
    }
    for (int i = 0; i < 2; i++) {
        if (!buffers[i]->busy) {
            buffer_destroy(buffers[i]);
            buffers[i] = buffer_create(bar);
            return buffers[i];
        }
    }
    return NULL;
}

static void render_frame(struct bar *bar) {
    if (!bar->configured) return;

    struct buffer *buf = get_buffer(bar);
    if (!buf) return;

    cairo_t *cr = cairo_create(buf->cairo_surface);
    render_content(bar, cr);
    cairo_destroy(cr);

    buf->busy = true;
    wl_surface_set_buffer_scale(bar->surface, bar->scale);
    wl_surface_attach(bar->surface, buf->wl_buf, 0, 0);
    wl_surface_damage_buffer(bar->surface, 0, 0,
                             bar->width * bar->scale,
                             bar->height * bar->scale);
    wl_surface_commit(bar->surface);
}

static void layer_configure(void *data, struct zwlr_layer_surface_v1 *lsurf,
                           uint32_t serial, uint32_t w, uint32_t h) {
    struct bar *bar = data;
    zwlr_layer_surface_v1_ack_configure(lsurf, serial);

    bar->width = (int)w > 0 ? (int)w : 1920;
    bar->height = (int)h > 0 ? (int)h : BAR_HEIGHT;
    bar->configured = true;

    clock_update(bar->clock);
    battery_update(bar->battery);
    volume_update(bar->volume);
    render_frame(bar);
}

static void layer_closed(void *data, struct zwlr_layer_surface_v1 *lsurf) {
    (void)lsurf;
    struct bar *bar = data;
    bar->running = 0;
}

static const struct zwlr_layer_surface_v1_listener layer_listener = {
    .configure = layer_configure,
    .closed = layer_closed,
};

struct bar *bar_create(void) {
    struct bar *bar = calloc(1, sizeof(*bar));
    if (!bar) return NULL;

    bar->display = wl_display_connect(NULL);
    if (!bar->display) {
        fprintf(stderr, "Failed to connect to Wayland\n");
        free(bar);
        return NULL;
    }

    bar->width = 1920;
    bar->height = BAR_HEIGHT;
    bar->scale = 2;

    registry_init(bar);

    if (!bar->compositor) {
        fprintf(stderr, "No wl_compositor\n");
        bar_destroy(bar);
        return NULL;
    }
    if (!bar->shm) {
        fprintf(stderr, "No wl_shm\n");
        bar_destroy(bar);
        return NULL;
    }
    if (!bar->layer_shell) {
        fprintf(stderr, "No zwlr_layer_shell_v1\n");
        bar_destroy(bar);
        return NULL;
    }

    bar->workspace_mgr = workspace_manager_create(bar);
    bar->battery = battery_create(bar);
    bar->volume = volume_create(bar);
    bar->clock = clock_create(bar);

    bar->surface = wl_compositor_create_surface(bar->compositor);

    bar->layer_surface = zwlr_layer_shell_v1_get_layer_surface(
        bar->layer_shell, bar->surface, NULL,
        ZWLR_LAYER_SURFACE_V1_LAYER_TOP, "simpbar");

    zwlr_layer_surface_v1_set_size(bar->layer_surface, 0, BAR_HEIGHT);
    zwlr_layer_surface_v1_set_anchor(bar->layer_surface,
        ZWLR_LAYER_SURFACE_V1_ANCHOR_BOTTOM |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_LEFT |
        ZWLR_LAYER_SURFACE_V1_ANCHOR_RIGHT);
    zwlr_layer_surface_v1_set_exclusive_zone(bar->layer_surface, BAR_HEIGHT);
    zwlr_layer_surface_v1_add_listener(bar->layer_surface, &layer_listener, bar);

    wl_surface_commit(bar->surface);
    wl_display_roundtrip(bar->display);

    return bar;
}

void bar_destroy(struct bar *bar) {
    if (!bar) return;

    buffer_destroy(buffers[0]); buffers[0] = NULL;
    buffer_destroy(buffers[1]); buffers[1] = NULL;

    if (bar->layer_surface) zwlr_layer_surface_v1_destroy(bar->layer_surface);
    if (bar->surface) wl_surface_destroy(bar->surface);
    if (bar->layer_shell) zwlr_layer_shell_v1_destroy(bar->layer_shell);
    if (bar->shm) wl_shm_destroy(bar->shm);
    if (bar->subcompositor) wl_subcompositor_destroy(bar->subcompositor);
    if (bar->compositor) wl_compositor_destroy(bar->compositor);
    if (bar->output) wl_output_destroy(bar->output);

    if (bar->clock) clock_destroy(bar->clock);
    if (bar->volume) volume_destroy(bar->volume);
    if (bar->battery) battery_destroy(bar->battery);
    if (bar->workspace_mgr) workspace_manager_destroy(bar->workspace_mgr);
    if (bar->workspace_proto) ext_workspace_manager_v1_stop(bar->workspace_proto);

    registry_fini(bar);

    if (bar->display) {
        wl_display_flush(bar->display);
        wl_display_disconnect(bar->display);
    }

    free(bar);
}

int bar_run(struct bar *bar) {
    bar->running = 1;
    time_t last_update = 0;

    while (bar->running) {
        if (wl_display_flush(bar->display) < 0 && errno != EAGAIN) break;

        struct pollfd pfd = {
            .fd = wl_display_get_fd(bar->display),
            .events = POLLIN,
        };

        if (poll(&pfd, 1, 1000) < 0) {
            if (errno == EINTR) continue;
            break;
        }

        if (pfd.revents & POLLIN) {
            if (wl_display_dispatch(bar->display) < 0) break;
        } else {
            wl_display_dispatch_pending(bar->display);
        }

        time_t now = time(NULL);
        if (now != last_update && bar->configured) {
            last_update = now;
            clock_update(bar->clock);
            battery_update(bar->battery);
            volume_update(bar->volume);
            render_frame(bar);
        }
    }

    return 0;
}

void bar_redraw(struct bar *bar) {
    if (bar && bar->configured) render_frame(bar);
}
