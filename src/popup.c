#include "popup.h"
#include "bar.h"
#include "battery.h"
#include "battery_history.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "xdg-shell-client-protocol.h"

#include <cairo/cairo.h>
#include <fcntl.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <time.h>
#include <unistd.h>

#define POPUP_WIDTH 256
#define POPUP_HEIGHT 156

struct popup {
    struct bar *bar;
    struct wl_surface *surface;
    struct xdg_surface *xdg_surface;
    struct xdg_popup *xdg_popup;

    struct wl_buffer *wl_buf;
    void *shm_data;
    size_t shm_size;
    cairo_surface_t *cairo_surface;
    int width, height;

    bool configured;
};

bool popup_owns_surface(struct popup *p, struct wl_surface *surface) {
    return p && surface && p->surface == surface;
}

static int create_shm_file(size_t size) {
    char name[] = "/simpbar-popup-XXXXXX";
    int fd = shm_open(name, O_RDWR | O_CREAT | O_EXCL, 0600);
    if (fd >= 0) {
        shm_unlink(name);
    } else {
        char path[] = "/tmp/simpbar-popup-XXXXXX";
        fd = mkstemp(path);
        if (fd >= 0) unlink(path);
    }
    if (fd < 0) return -1;
    if (ftruncate(fd, size) < 0) { close(fd); return -1; }
    return fd;
}

static void render_chart(struct popup *p, cairo_t *cr) {
    int w = p->width;
    int h = p->height;

    /* Background */
    cairo_set_operator(cr, CAIRO_OPERATOR_SOURCE);
    cairo_set_source_rgba(cr, 0.08, 0.08, 0.10, 0.96);
    cairo_paint(cr);
    cairo_set_operator(cr, CAIRO_OPERATOR_OVER);

    /* Border */
    cairo_set_line_width(cr, 1.0);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.10);
    cairo_rectangle(cr, 0.5, 0.5, w - 1, h - 1);
    cairo_stroke(cr);

    cairo_select_font_face(cr, "sans-serif",
        CAIRO_FONT_SLANT_NORMAL, CAIRO_FONT_WEIGHT_NORMAL);

    /* Chart area */
    int chart_left = 18;
    int chart_right = w - 18;
    int chart_top = 18;
    int chart_bottom = h - 54;
    int chart_w = chart_right - chart_left;
    int chart_h = chart_bottom - chart_top;

    /* Baseline */
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.12);
    cairo_move_to(cr, chart_left, chart_bottom + 0.5);
    cairo_line_to(cr, chart_right, chart_bottom + 0.5);
    cairo_stroke(cr);

    struct hour_bucket buckets[HISTORY_HOURS];
    battery_history_buckets(p->bar->battery_history, buckets);

    double slot_w = (double)chart_w / HISTORY_HOURS;
    double inner_w = 6.0;
    if (inner_w > slot_w - 2.0) inner_w = slot_w - 2.0;
    if (inner_w < 1.0) inner_w = 1.0;

    for (int i = 0; i < HISTORY_HOURS; i++) {
        double bx = chart_left + i * slot_w + (slot_w - inner_w) / 2.0;
        struct hour_bucket *b = &buckets[i];
        if (!b->has_data) continue;

        double pct = b->percentage / 100.0;
        double bh = chart_h * pct;
        if (bh < 2.0) bh = 2.0;
        double by = chart_bottom - bh;

        switch (b->color) {
            case HOUR_GREEN:  cairo_set_source_rgb(cr, 0.40, 0.78, 0.45); break;
            case HOUR_YELLOW: cairo_set_source_rgb(cr, 0.92, 0.78, 0.30); break;
            case HOUR_RED:    cairo_set_source_rgb(cr, 0.90, 0.35, 0.35); break;
            case HOUR_SLEEP:  cairo_set_source_rgb(cr, 0.55, 0.45, 0.78); break;
            default:          cairo_set_source_rgba(cr, 1, 1, 1, 0.15); break;
        }

        double r = inner_w / 2.0;
        if (r > 2.0) r = 2.0;
        if (r > bh) r = bh;
        cairo_new_sub_path(cr);
        cairo_arc(cr, bx + inner_w - r, by + r, r, -M_PI/2, 0);
        cairo_line_to(cr, bx + inner_w, by + bh);
        cairo_line_to(cr, bx, by + bh);
        cairo_line_to(cr, bx, by + r);
        cairo_arc(cr, bx + r, by + r, r, M_PI, 3*M_PI/2);
        cairo_close_path(cr);
        cairo_fill(cr);
    }

    /* X-axis label: -24h ... now */
    cairo_set_font_size(cr, 10);
    cairo_set_source_rgba(cr, 1.0, 1.0, 1.0, 0.45);
    cairo_text_extents_t te;

    cairo_text_extents(cr, "-24h", &te);
    cairo_move_to(cr, chart_left, chart_bottom + 12);
    cairo_show_text(cr, "-24h");

    cairo_text_extents(cr, "now", &te);
    cairo_move_to(cr, chart_right - te.width, chart_bottom + 12);
    cairo_show_text(cr, "now");

    /* Autonomy text — live battery state is authoritative; only fall back to
     * history when we're actually on battery. */
    char auton_buf[128];
    enum battery_state bst = p->bar->battery->state;
    if (bst == BATTERY_CHARGING) {
        snprintf(auton_buf, sizeof(auton_buf), "Currently charging");
    } else if (bst == BATTERY_FULL) {
        snprintf(auton_buf, sizeof(auton_buf), "Plugged in — fully charged");
    } else {
        int64_t autonomy = battery_history_autonomy_secs(p->bar->battery_history);
        if (autonomy < 0) {
            snprintf(auton_buf, sizeof(auton_buf), "Autonomy: no data yet");
        } else {
            int hours = (int)(autonomy / 3600);
            int mins = (int)((autonomy % 3600) / 60);
            snprintf(auton_buf, sizeof(auton_buf),
                     "Autonomy since last charge: %dh %02dmin", hours, mins);
        }
    }
    cairo_set_font_size(cr, 12);
    cairo_set_source_rgba(cr, 0.92, 0.92, 0.94, 0.95);
    cairo_text_extents(cr, auton_buf, &te);
    cairo_move_to(cr, (w - te.width) / 2.0, h - 18);
    cairo_show_text(cr, auton_buf);
}

static void create_buffer(struct popup *p) {
    int pw = p->width * p->bar->scale;
    int ph = p->height * p->bar->scale;
    int stride = cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32, pw);
    size_t size = stride * ph;

    int fd = create_shm_file(size);
    if (fd < 0) return;

    void *data = mmap(NULL, size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (data == MAP_FAILED) { close(fd); return; }

    struct wl_shm_pool *pool = wl_shm_create_pool(p->bar->shm, fd, size);
    p->wl_buf = wl_shm_pool_create_buffer(pool, 0, pw, ph, stride,
                                          WL_SHM_FORMAT_ARGB8888);
    wl_shm_pool_destroy(pool);
    close(fd);

    p->shm_data = data;
    p->shm_size = size;
    p->cairo_surface = cairo_image_surface_create_for_data(
        data, CAIRO_FORMAT_ARGB32, pw, ph, stride);
    cairo_surface_set_device_scale(p->cairo_surface, p->bar->scale, p->bar->scale);
}

void popup_redraw(struct popup *p) {
    if (!p || !p->configured || !p->cairo_surface) return;

    cairo_t *cr = cairo_create(p->cairo_surface);
    render_chart(p, cr);
    cairo_destroy(cr);

    wl_surface_set_buffer_scale(p->surface, p->bar->scale);
    wl_surface_attach(p->surface, p->wl_buf, 0, 0);
    wl_surface_damage_buffer(p->surface, 0, 0,
                             p->width * p->bar->scale,
                             p->height * p->bar->scale);
    wl_surface_commit(p->surface);
}

static void xdg_surface_configure(void *data, struct xdg_surface *xdg_surface,
                                  uint32_t serial) {
    struct popup *p = data;
    xdg_surface_ack_configure(xdg_surface, serial);

    if (!p->wl_buf) create_buffer(p);
    p->configured = true;
    popup_redraw(p);
}

static const struct xdg_surface_listener xdg_surface_listener = {
    .configure = xdg_surface_configure,
};

static void xdg_popup_configure(void *data, struct xdg_popup *xdg_popup,
                                int32_t x, int32_t y, int32_t w, int32_t h) {
    (void)data; (void)xdg_popup; (void)x; (void)y; (void)w; (void)h;
}

static void xdg_popup_done(void *data, struct xdg_popup *xdg_popup) {
    (void)xdg_popup;
    struct popup *p = data;
    /* Compositor dismissed the popup. Bar will detect this by checking
     * pointer state on the next event; just mark it as dead for redraw. */
    p->configured = false;
}

static void xdg_popup_repositioned(void *data, struct xdg_popup *xdg_popup,
                                   uint32_t token) {
    (void)data; (void)xdg_popup; (void)token;
}

static const struct xdg_popup_listener xdg_popup_listener = {
    .configure = xdg_popup_configure,
    .popup_done = xdg_popup_done,
    .repositioned = xdg_popup_repositioned,
};

struct popup *popup_create_battery(struct bar *bar,
                                   double anchor_x, double anchor_y,
                                   double anchor_w, double anchor_h) {
    if (!bar->xdg_wm_base || !bar->layer_surface) return NULL;

    struct popup *p = calloc(1, sizeof(*p));
    if (!p) return NULL;
    p->bar = bar;
    p->width = POPUP_WIDTH;
    p->height = POPUP_HEIGHT;

    p->surface = wl_compositor_create_surface(bar->compositor);

    struct xdg_positioner *pos = xdg_wm_base_create_positioner(bar->xdg_wm_base);
    xdg_positioner_set_size(pos, p->width, p->height);
    xdg_positioner_set_anchor_rect(pos,
                                   (int)anchor_x, (int)anchor_y,
                                   (int)anchor_w, (int)anchor_h);
    xdg_positioner_set_anchor(pos, XDG_POSITIONER_ANCHOR_TOP);
    xdg_positioner_set_gravity(pos, XDG_POSITIONER_GRAVITY_TOP);
    xdg_positioner_set_offset(pos, 0, -6);
    xdg_positioner_set_constraint_adjustment(pos,
        XDG_POSITIONER_CONSTRAINT_ADJUSTMENT_SLIDE_X);

    p->xdg_surface = xdg_wm_base_get_xdg_surface(bar->xdg_wm_base, p->surface);
    xdg_surface_add_listener(p->xdg_surface, &xdg_surface_listener, p);

    p->xdg_popup = xdg_surface_get_popup(p->xdg_surface, NULL, pos);
    xdg_popup_add_listener(p->xdg_popup, &xdg_popup_listener, p);
    xdg_positioner_destroy(pos);

    zwlr_layer_surface_v1_get_popup(bar->layer_surface, p->xdg_popup);

    wl_surface_commit(p->surface);
    return p;
}

void popup_destroy(struct popup *p) {
    if (!p) return;
    if (p->cairo_surface) cairo_surface_destroy(p->cairo_surface);
    if (p->shm_data) munmap(p->shm_data, p->shm_size);
    if (p->wl_buf) wl_buffer_destroy(p->wl_buf);
    if (p->xdg_popup) xdg_popup_destroy(p->xdg_popup);
    if (p->xdg_surface) xdg_surface_destroy(p->xdg_surface);
    if (p->surface) wl_surface_destroy(p->surface);
    free(p);
}
