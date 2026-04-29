#include "bar.h"
#include "registry.h"
#include "workspace.h"
#include "toplevel.h"
#include "battery.h"
#include "volume.h"
#include "backlight.h"
#include "clock.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"
#include <wayland-cursor.h>

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
#include <sys/timerfd.h>
#include <cairo/cairo.h>

#define BAR_HEIGHT 32

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
    struct bar *bar;
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

    /* If a redraw was requested while both buffers were busy, render now. */
    if (buf->bar && buf->bar->pending_redraw) {
        buf->bar->pending_redraw = false;
        bar_redraw(buf->bar);
    }
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
    buf->bar = bar;
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
    double font_sz = 14;
    double section_gap = 18;

    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgb(cr, 0.0, 0.0, 0.0);
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
    double pct_width = ext.width;

    // Build time-remaining string (drawn after percentage, to its right, no parens)
    char time_buf[32] = "";
    if (bar->battery->time_remaining >= 0) {
        int mins = bar->battery->time_remaining;
        double hours = mins / 60.0;
        int whole = (int)hours;
        double frac = hours - whole;

        const char *frac_str = "";
        if (frac >= 0.875) { whole++; }
        else if (frac >= 0.625) { frac_str = "\xC2\xBE"; }   // ¾
        else if (frac >= 0.375) { frac_str = "\xC2\xBD"; }   // ½
        else if (frac >= 0.125) { frac_str = "\xC2\xBC"; }   // ¼

        if (whole > 0) {
            snprintf(time_buf, sizeof(time_buf), " %d%sh", whole, frac_str);
        } else {
            int rem_mins = (int)(frac * 60.0 + 0.5);
            if (rem_mins > 0) {
                snprintf(time_buf, sizeof(time_buf), " %dm", rem_mins);
            }
        }
    }

    // Fixed-width time area so icons don't shift when fraction changes
    char time_ref[] = " 9\xC2\xBEh";
    cairo_text_extents_t time_ref_ext;
    cairo_text_extents(cr, time_ref, &time_ref_ext);
    double time_reserved = time_buf[0] ? time_ref_ext.width : 0;

    double total_width = pct_width + time_reserved;
    rx -= total_width;
    double text_start = rx;

    double br, bg, bb;
    if (pct < 10)      { br = 0.90; bg = 0.30; bb = 0.30; }
    else if (pct < 30) { br = 0.85; bg = 0.75; bb = 0.30; }
    else               { br = 0.88; bg = 0.88; bb = 0.90; }

    // Draw percentage
    cairo_set_source_rgb(cr, br, bg, bb);
    cairo_move_to(cr, text_start, text_y);
    cairo_show_text(cr, buf);

    // Draw time remaining to the right of percentage (fixed-width zone)
    if (time_buf[0]) {
        cairo_set_source_rgb(cr, 0.55, 0.55, 0.58);
        cairo_move_to(cr, text_start + pct_width, text_y);
        cairo_show_text(cr, time_buf);
    }

    rx -= 5;

    {
        double bw = 18, bh = 9;
        double ix = rx - bw - 2, iy = cy;
        cairo_set_line_width(cr, 1.0);
        cairo_set_source_rgb(cr, 0.6, 0.6, 0.6);
        cairo_rectangle(cr, ix, iy - bh/2, bw, bh);
        cairo_stroke(cr);
        cairo_rectangle(cr, ix + bw, iy - 1.5, 2, 3);
        cairo_fill(cr);
        double fill_w = (bw - 2) * (pct / 100.0);
        cairo_set_source_rgb(cr, br, bg, bb);
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

    // Volume icon (level shown transiently in the center on change)
    int vol = bar->volume->level;
    {
        double ix = rx - 14, iy = cy;
        cairo_set_source_rgb(cr, 0.88, 0.88, 0.90);
        cairo_rectangle(cr, ix, iy - 3, 4, 6);
        cairo_fill(cr);
        cairo_move_to(cr, ix + 4, iy - 3);
        cairo_line_to(cr, ix + 8.5, iy - 6);
        cairo_line_to(cr, ix + 8.5, iy + 6);
        cairo_line_to(cr, ix + 4, iy + 3);
        cairo_close_path(cr);
        cairo_fill(cr);
        if (bar->volume->muted) {
            cairo_set_line_width(cr, 1.4);
            cairo_move_to(cr, ix + 10, iy - 4);
            cairo_line_to(cr, ix + 16, iy + 4);
            cairo_move_to(cr, ix + 16, iy - 4);
            cairo_line_to(cr, ix + 10, iy + 4);
            cairo_stroke(cr);
        } else {
            cairo_set_line_width(cr, 1.2);
            if (vol > 33) {
                cairo_arc(cr, ix + 9.5, iy, 4, -M_PI/4, M_PI/4);
                cairo_stroke(cr);
            }
            if (vol > 66) {
                cairo_arc(cr, ix + 9.5, iy, 7, -M_PI/4, M_PI/4);
                cairo_stroke(cr);
            }
        }
        rx = ix - section_gap;
    }

    // Centered transient indicators (brightness, volume) — appear on change
    {
        bool bl_vis  = backlight_visible(bar->backlight);
        bool vol_vis = volume_visible(bar->volume);
        if (bl_vis || vol_vis) {
            double icon_w = 14, gap = 6, pw = 46, ph = 6, pr = ph / 2.0;
            double widget_w = icon_w + gap + pw;
            double inter_gap = 18;
            double total = widget_w + (bl_vis && vol_vis ? widget_w + inter_gap : 0);
            double bx = (w - total) / 2.0;

            if (bl_vis) {
                double frac = backlight_perceptual_fraction(bar->backlight);
                double sx = bx + 7, iy = cy;
                cairo_set_source_rgb(cr, 0.88, 0.88, 0.90);
                cairo_arc(cr, sx, iy, 3.0, 0, 2 * M_PI);
                cairo_fill(cr);
                cairo_set_line_width(cr, 1.2);
                cairo_set_line_cap(cr, CAIRO_LINE_CAP_ROUND);
                for (int i = 0; i < 8; i++) {
                    double a = i * (M_PI / 4.0);
                    double r0 = 4.5, r1 = 6.5;
                    cairo_move_to(cr, sx + cos(a) * r0, iy + sin(a) * r0);
                    cairo_line_to(cr, sx + cos(a) * r1, iy + sin(a) * r1);
                }
                cairo_stroke(cr);

                double px = bx + icon_w + gap;
                double py = cy - ph / 2.0;
                cairo_set_source_rgba(cr, 0.88, 0.88, 0.90, 0.30);
                cairo_new_sub_path(cr);
                cairo_arc(cr, px + pw - pr, py + pr, pr, -M_PI/2, M_PI/2);
                cairo_arc(cr, px + pr,      py + pr, pr,  M_PI/2, 3*M_PI/2);
                cairo_close_path(cr);
                cairo_fill(cr);

                double fw = pw * frac;
                if (fw > 0) {
                    if (fw < ph) fw = ph;
                    cairo_set_source_rgb(cr, 0.95, 0.95, 0.97);
                    cairo_new_sub_path(cr);
                    cairo_arc(cr, px + fw - pr, py + pr, pr, -M_PI/2, M_PI/2);
                    cairo_arc(cr, px + pr,      py + pr, pr,  M_PI/2, 3*M_PI/2);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                }
                bx += widget_w + inter_gap;
            }

            if (vol_vis) {
                double frac = bar->volume->muted ? 0.0 : (vol / 100.0);
                double sx = bx, iy = cy;
                cairo_set_source_rgb(cr, 0.88, 0.88, 0.90);
                cairo_rectangle(cr, sx, iy - 3, 4, 6);
                cairo_fill(cr);
                cairo_move_to(cr, sx + 4, iy - 3);
                cairo_line_to(cr, sx + 8.5, iy - 6);
                cairo_line_to(cr, sx + 8.5, iy + 6);
                cairo_line_to(cr, sx + 4, iy + 3);
                cairo_close_path(cr);
                cairo_fill(cr);
                if (bar->volume->muted) {
                    cairo_set_line_width(cr, 1.4);
                    cairo_move_to(cr, sx + 10, iy - 4);
                    cairo_line_to(cr, sx + 16, iy + 4);
                    cairo_move_to(cr, sx + 16, iy - 4);
                    cairo_line_to(cr, sx + 10, iy + 4);
                    cairo_stroke(cr);
                }

                double px = bx + icon_w + gap;
                double py = cy - ph / 2.0;
                cairo_set_source_rgba(cr, 0.88, 0.88, 0.90, 0.30);
                cairo_new_sub_path(cr);
                cairo_arc(cr, px + pw - pr, py + pr, pr, -M_PI/2, M_PI/2);
                cairo_arc(cr, px + pr,      py + pr, pr,  M_PI/2, 3*M_PI/2);
                cairo_close_path(cr);
                cairo_fill(cr);

                double fw = pw * frac;
                if (fw > 0) {
                    if (fw < ph) fw = ph;
                    cairo_set_source_rgb(cr, 0.95, 0.95, 0.97);
                    cairo_new_sub_path(cr);
                    cairo_arc(cr, px + fw - pr, py + pr, pr, -M_PI/2, M_PI/2);
                    cairo_arc(cr, px + pr,      py + pr, pr,  M_PI/2, 3*M_PI/2);
                    cairo_close_path(cr);
                    cairo_fill(cr);
                }
            }
        }
    }

    // Workspace indicators — neutral colors, right-to-left before icons
    for (int i = bar->workspace_mgr->workspace_count - 1; i >= 0 && i < 8; i--) {
        if (bar->workspace_mgr->workspaces[i].state == WORKSPACE_ACTIVE) {
            cairo_set_source_rgb(cr, 0.80, 0.80, 0.82);
            double rw = 26, rh = 5, radius = 2.5;
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
            cairo_arc(cr, rx + 3.5, cy, 3.5, 0, 2 * M_PI);
            cairo_fill(cr);
            rx -= 10;
        }
    }

    // Running applications (left side, stable order)
    {
        double lx = 14;
        double icon_draw_sz = h - 10;
        double icon_gap = 4;

        for (int si = 0; si < bar->toplevel_mgr->sorted_count; si++) {
            int idx = bar->toplevel_mgr->sorted_indices[si];
            struct toplevel_info *tl = &bar->toplevel_mgr->toplevels[idx];
            if (!tl->app_id[0] && !tl->title[0]) continue;

            if (lx + icon_draw_sz > rx - 20) break;

            tl->render_x = lx;
            tl->render_w = icon_draw_sz;

            if (tl->activated) {
                cairo_set_source_rgb(cr, 0.95, 0.95, 0.97);
                double pw = 12, ph = 2.5, pr = ph / 2.0;
                double px = lx + (icon_draw_sz - pw) / 2.0;
                double py = h - ph - 1.5;
                cairo_new_sub_path(cr);
                cairo_arc(cr, px + pw - pr, py + pr, pr, -M_PI/2, M_PI/2);
                cairo_arc(cr, px + pr, py + pr, pr, M_PI/2, 3*M_PI/2);
                cairo_close_path(cr);
                cairo_fill(cr);
            }

            /* Reduced opacity for minimized windows */
            if (tl->minimized) {
                cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.5);
            }

            if (tl->icon) {
                int iw = cairo_image_surface_get_width(tl->icon);
                double scale = icon_draw_sz / iw;
                cairo_save(cr);
                cairo_translate(cr, lx, (h - icon_draw_sz) / 2.0);
                cairo_scale(cr, scale, scale);
                cairo_set_source_surface(cr, tl->icon, 0, 0);
                cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
                if (tl->minimized) {
                    cairo_paint_with_alpha(cr, 0.5);
                } else {
                    cairo_paint(cr);
                }
                cairo_restore(cr);
            } else {
                const char *label = tl->app_id[0] ? tl->app_id : tl->title;
                char initial[4] = {0};
                initial[0] = (label[0] >= 'a' && label[0] <= 'z')
                    ? label[0] - 32 : label[0];

                cairo_set_source_rgb(cr, 0.20, 0.20, 0.24);
                double r = icon_draw_sz / 2.0;
                cairo_arc(cr, lx + r, cy, r, 0, 2 * M_PI);
                cairo_fill(cr);

                cairo_set_source_rgb(cr, 0.75, 0.75, 0.78);
                cairo_set_font_size(cr, icon_draw_sz * 0.55);
                cairo_text_extents_t ie;
                cairo_text_extents(cr, initial, &ie);
                cairo_move_to(cr, lx + r - ie.width/2 - ie.x_bearing,
                              cy - ie.height/2 - ie.y_bearing);
                cairo_show_text(cr, initial);
                cairo_set_font_size(cr, font_sz);
            }

            lx += icon_draw_sz + icon_gap;
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
    if (!buf) {
        /* Both buffers busy, render on next release callback immediately. */
        bar->pending_redraw = true;
        return;
    }

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
    backlight_update(bar->backlight);
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

// Pointer events for click handling
static void pointer_enter(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface,
                          wl_fixed_t sx, wl_fixed_t sy) {
    (void)surface;
    struct bar *bar = data;
    bar->pointer_inside = true;
    bar->pointer_x = wl_fixed_to_double(sx);
    bar->pointer_y = wl_fixed_to_double(sy);

    if (bar->cursor_theme && bar->cursor_surface) {
        struct wl_cursor *cursor = wl_cursor_theme_get_cursor(bar->cursor_theme, "default");
        if (!cursor) cursor = wl_cursor_theme_get_cursor(bar->cursor_theme, "left_ptr");
        if (cursor && cursor->image_count > 0) {
            struct wl_cursor_image *img = cursor->images[0];
            struct wl_buffer *buf = wl_cursor_image_get_buffer(img);
            wl_surface_set_buffer_scale(bar->cursor_surface, bar->scale);
            wl_surface_attach(bar->cursor_surface, buf, 0, 0);
            wl_surface_damage_buffer(bar->cursor_surface, 0, 0, img->width, img->height);
            wl_surface_commit(bar->cursor_surface);
            wl_pointer_set_cursor(pointer, serial, bar->cursor_surface,
                                  img->hotspot_x / bar->scale, img->hotspot_y / bar->scale);
        }
    }
}

static void pointer_leave(void *data, struct wl_pointer *pointer,
                          uint32_t serial, struct wl_surface *surface) {
    (void)pointer; (void)serial; (void)surface;
    struct bar *bar = data;
    bar->pointer_inside = false;
}

static void pointer_motion(void *data, struct wl_pointer *pointer,
                           uint32_t time, wl_fixed_t sx, wl_fixed_t sy) {
    (void)pointer; (void)time;
    struct bar *bar = data;
    bar->pointer_x = wl_fixed_to_double(sx);
    bar->pointer_y = wl_fixed_to_double(sy);
}

static void pointer_button(void *data, struct wl_pointer *pointer,
                           uint32_t serial, uint32_t time, uint32_t button,
                           uint32_t state) {
    (void)pointer; (void)serial; (void)time;
    struct bar *bar = data;

    if (state != WL_POINTER_BUTTON_STATE_PRESSED) return;
    if (button != 0x110) return;  // BTN_LEFT

    if (!bar->toplevel_mgr || !bar->seat) return;

    struct toplevel_info *tl = toplevel_at_x(bar->toplevel_mgr, bar->pointer_x);
    if (tl && tl->handle) {
        zwlr_foreign_toplevel_handle_v1_activate(tl->handle, bar->seat);
    }
}

static void pointer_axis(void *data, struct wl_pointer *pointer,
                         uint32_t time, uint32_t axis, wl_fixed_t value) {
    (void)data; (void)pointer; (void)time; (void)axis; (void)value;
}

static void pointer_frame(void *data, struct wl_pointer *pointer) {
    (void)data; (void)pointer;
}

static void pointer_axis_source(void *data, struct wl_pointer *pointer,
                                uint32_t source) {
    (void)data; (void)pointer; (void)source;
}

static void pointer_axis_stop(void *data, struct wl_pointer *pointer,
                              uint32_t time, uint32_t axis) {
    (void)data; (void)pointer; (void)time; (void)axis;
}

static void pointer_axis_discrete(void *data, struct wl_pointer *pointer,
                                  uint32_t axis, int32_t discrete) {
    (void)data; (void)pointer; (void)axis; (void)discrete;
}

static const struct wl_pointer_listener pointer_listener = {
    .enter = pointer_enter,
    .leave = pointer_leave,
    .motion = pointer_motion,
    .button = pointer_button,
    .axis = pointer_axis,
    .frame = pointer_frame,
    .axis_source = pointer_axis_source,
    .axis_stop = pointer_axis_stop,
    .axis_discrete = pointer_axis_discrete,
};

static void seat_capabilities(void *data, struct wl_seat *seat, uint32_t caps) {
    struct bar *bar = data;
    if ((caps & WL_SEAT_CAPABILITY_POINTER) && !bar->pointer) {
        bar->pointer = wl_seat_get_pointer(seat);
        wl_pointer_add_listener(bar->pointer, &pointer_listener, bar);
    }
    if (!(caps & WL_SEAT_CAPABILITY_POINTER) && bar->pointer) {
        wl_pointer_destroy(bar->pointer);
        bar->pointer = NULL;
    }
}

static void seat_name(void *data, struct wl_seat *seat, const char *name) {
    (void)data; (void)seat; (void)name;
}

static const struct wl_seat_listener seat_listener = {
    .capabilities = seat_capabilities,
    .name = seat_name,
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

    if (bar->seat) {
        wl_seat_add_listener(bar->seat, &seat_listener, bar);
    }

    bar->workspace_mgr = workspace_manager_create(bar);
    bar->toplevel_mgr = toplevel_manager_create(bar);
    wl_display_roundtrip(bar->display);

    bar->battery = battery_create(bar);
    bar->volume = volume_create(bar);
    bar->backlight = backlight_create(bar);
    bar->clock = clock_create(bar);

    bar->cursor_theme = wl_cursor_theme_load(NULL, 24 * bar->scale, bar->shm);
    bar->cursor_surface = wl_compositor_create_surface(bar->compositor);

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

    if (bar->cursor_surface) wl_surface_destroy(bar->cursor_surface);
    if (bar->cursor_theme) wl_cursor_theme_destroy(bar->cursor_theme);
    if (bar->pointer) wl_pointer_destroy(bar->pointer);
    if (bar->layer_surface) zwlr_layer_surface_v1_destroy(bar->layer_surface);
    if (bar->surface) wl_surface_destroy(bar->surface);
    if (bar->layer_shell) zwlr_layer_shell_v1_destroy(bar->layer_shell);
    if (bar->shm) wl_shm_destroy(bar->shm);
    if (bar->seat) wl_seat_destroy(bar->seat);
    if (bar->subcompositor) wl_subcompositor_destroy(bar->subcompositor);
    if (bar->compositor) wl_compositor_destroy(bar->compositor);
    if (bar->output) wl_output_destroy(bar->output);

    if (bar->clock) clock_destroy(bar->clock);
    if (bar->backlight) backlight_destroy(bar->backlight);
    if (bar->volume) volume_destroy(bar->volume);
    if (bar->battery) battery_destroy(bar->battery);
    if (bar->toplevel_mgr) toplevel_manager_destroy(bar->toplevel_mgr);
    if (bar->workspace_mgr) workspace_manager_destroy(bar->workspace_mgr);
    if (bar->toplevel_mgr_proto) zwlr_foreign_toplevel_manager_v1_stop(bar->toplevel_mgr_proto);
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

    int wl_fd = wl_display_get_fd(bar->display);
    int vol_fd = volume_get_fd(bar->volume);
    int bl_fd = backlight_get_fd(bar->backlight);
    int timer_fd = timerfd_create(CLOCK_MONOTONIC, TFD_NONBLOCK | TFD_CLOEXEC);
    if (timer_fd >= 0) {
        struct itimerspec ts = {
            .it_interval = { .tv_sec = 1, .tv_nsec = 0 },
            .it_value    = { .tv_sec = 1, .tv_nsec = 0 },
        };
        timerfd_settime(timer_fd, 0, &ts, NULL);
    }

    enum { SRC_WL = 0, SRC_VOL, SRC_BL, SRC_TIMER, SRC_COUNT };
    struct pollfd pfds[SRC_COUNT];
    int idx[SRC_COUNT] = { -1, -1, -1, -1 };
    int nfds = 0;

    pfds[nfds].fd = wl_fd; pfds[nfds].events = POLLIN; idx[SRC_WL] = nfds++;
    if (vol_fd >= 0)   { pfds[nfds].fd = vol_fd;   pfds[nfds].events = POLLIN; idx[SRC_VOL]   = nfds++; }
    if (bl_fd >= 0)    { pfds[nfds].fd = bl_fd;    pfds[nfds].events = POLLIN; idx[SRC_BL]    = nfds++; }
    if (timer_fd >= 0) { pfds[nfds].fd = timer_fd; pfds[nfds].events = POLLIN; idx[SRC_TIMER] = nfds++; }

    bool bl_was_visible = false;
    bool vol_was_visible = false;

    while (bar->running) {
        while (wl_display_prepare_read(bar->display) != 0) {
            wl_display_dispatch_pending(bar->display);
        }
        wl_display_flush(bar->display);

        /* Wake at the earliest indicator hide deadline so it disappears promptly. */
        int poll_timeout = -1;
        if (bl_was_visible) {
            int rem = backlight_remaining_ms(bar->backlight);
            poll_timeout = rem >= 0 ? rem : 0;
        }
        if (vol_was_visible) {
            int rem = volume_remaining_ms(bar->volume);
            int t = rem >= 0 ? rem : 0;
            if (poll_timeout < 0 || t < poll_timeout) poll_timeout = t;
        }

        int n = poll(pfds, nfds, poll_timeout);
        if (n < 0) {
            wl_display_cancel_read(bar->display);
            if (errno == EINTR) continue;
            break;
        }

        if (pfds[idx[SRC_WL]].revents & POLLIN) {
            if (wl_display_read_events(bar->display) < 0) break;
            wl_display_dispatch_pending(bar->display);
        } else {
            wl_display_cancel_read(bar->display);
        }
        if (pfds[idx[SRC_WL]].revents & (POLLERR | POLLHUP)) break;

        if (idx[SRC_VOL] >= 0 && (pfds[idx[SRC_VOL]].revents & POLLIN)) {
            if (volume_dispatch(bar->volume) && bar->configured) {
                render_frame(bar);
            }
        }

        if (idx[SRC_BL] >= 0 && (pfds[idx[SRC_BL]].revents & POLLIN)) {
            if (backlight_dispatch(bar->backlight) && bar->configured) {
                render_frame(bar);
            }
        }

        if (idx[SRC_TIMER] >= 0 && (pfds[idx[SRC_TIMER]].revents & POLLIN)) {
            uint64_t exp;
            ssize_t r = read(timer_fd, &exp, sizeof(exp));
            (void)r;
            if (bar->configured) {
                clock_update(bar->clock);
                battery_update(bar->battery);
                render_frame(bar);
            }
        }

        /* If a transient indicator just appeared or disappeared (e.g. its
         * hide timeout elapsed), redraw to reflect the change. */
        bool bl_visible_now  = backlight_visible(bar->backlight);
        bool vol_visible_now = volume_visible(bar->volume);
        if ((bl_visible_now != bl_was_visible ||
             vol_visible_now != vol_was_visible) && bar->configured) {
            render_frame(bar);
        }
        bl_was_visible = bl_visible_now;
        vol_was_visible = vol_visible_now;
    }

    if (timer_fd >= 0) close(timer_fd);
    return 0;
}

void bar_redraw(struct bar *bar) {
    if (bar && bar->configured) render_frame(bar);
}
