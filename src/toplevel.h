#ifndef TOPLEVEL_H
#define TOPLEVEL_H

#include <stdbool.h>
#include <stdint.h>
#include <cairo/cairo.h>

#define MAX_TOPLEVELS 64

struct bar;

struct toplevel_info {
    struct zwlr_foreign_toplevel_handle_v1 *handle;
    char app_id[128];
    char title[256];
    cairo_surface_t *icon;
    bool icon_loaded;
    bool activated;
    uint64_t focus_seq;
    double render_x;
    double render_w;
};

struct toplevel_manager {
    struct bar *bar;
    int count;
    int icon_size;
    uint64_t focus_counter;
    struct toplevel_info toplevels[MAX_TOPLEVELS];
    int sorted_indices[MAX_TOPLEVELS];
    int sorted_count;
};

struct toplevel_manager *toplevel_manager_create(struct bar *bar);
void toplevel_manager_destroy(struct toplevel_manager *mgr);
void toplevel_manager_sort(struct toplevel_manager *mgr);
struct toplevel_info *toplevel_at_x(struct toplevel_manager *mgr, double x);

#endif
