#include "toplevel.h"
#include "icon_loader.h"
#include "bar.h"
#include "wlr-foreign-toplevel-management-unstable-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct toplevel_info *find_by_handle(
    struct toplevel_manager *mgr, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->toplevels[i].handle == handle) return &mgr->toplevels[i];
    }
    return NULL;
}

static void handle_title(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                         const char *title) {
    struct toplevel_manager *mgr = data;
    struct toplevel_info *tl = find_by_handle(mgr, handle);
    if (tl && title) strncpy(tl->title, title, sizeof(tl->title) - 1);
}

static void handle_app_id(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                          const char *app_id) {
    struct toplevel_manager *mgr = data;
    struct toplevel_info *tl = find_by_handle(mgr, handle);
    if (!tl || !app_id) return;

    if (strcmp(tl->app_id, app_id) == 0 && tl->icon_loaded) return;

    strncpy(tl->app_id, app_id, sizeof(tl->app_id) - 1);

    if (tl->icon) {
        cairo_surface_destroy(tl->icon);
        tl->icon = NULL;
    }

    tl->icon = icon_load(app_id, mgr->icon_size);
    tl->icon_loaded = true;
}

static void handle_output_enter(void *data,
                                struct zwlr_foreign_toplevel_handle_v1 *handle,
                                struct wl_output *output) {
    (void)output;
    struct toplevel_manager *mgr = data;
    struct toplevel_info *tl = find_by_handle(mgr, handle);
    if (tl) tl->output_count++;
}

static void handle_output_leave(void *data,
                                struct zwlr_foreign_toplevel_handle_v1 *handle,
                                struct wl_output *output) {
    (void)output;
    struct toplevel_manager *mgr = data;
    struct toplevel_info *tl = find_by_handle(mgr, handle);
    if (tl && tl->output_count > 0) tl->output_count--;
}

static void handle_state(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                         struct wl_array *state) {
    struct toplevel_manager *mgr = data;
    struct toplevel_info *tl = find_by_handle(mgr, handle);
    if (!tl) return;

    bool was_activated = tl->activated;
    tl->activated = false;

    uint32_t *s;
    wl_array_for_each(s, state) {
        if (*s == ZWLR_FOREIGN_TOPLEVEL_HANDLE_V1_STATE_ACTIVATED)
            tl->activated = true;
    }

    if (tl->activated && !was_activated) {
        tl->focus_seq = ++mgr->focus_counter;
    }
}

static void handle_done(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)handle;
    struct toplevel_manager *mgr = data;
    toplevel_manager_sort(mgr);
    bar_redraw(mgr->bar);
}

static void handle_closed(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle) {
    struct toplevel_manager *mgr = data;
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->toplevels[i].handle == handle) {
            if (mgr->toplevels[i].icon)
                cairo_surface_destroy(mgr->toplevels[i].icon);
            zwlr_foreign_toplevel_handle_v1_destroy(handle);
            for (int j = i; j < mgr->count - 1; j++)
                mgr->toplevels[j] = mgr->toplevels[j + 1];
            mgr->count--;
            toplevel_manager_sort(mgr);
            bar_redraw(mgr->bar);
            return;
        }
    }
}

static void handle_parent(void *data, struct zwlr_foreign_toplevel_handle_v1 *handle,
                          struct zwlr_foreign_toplevel_handle_v1 *parent) {
    (void)data; (void)handle; (void)parent;
}

static const struct zwlr_foreign_toplevel_handle_v1_listener handle_listener = {
    .title = handle_title,
    .app_id = handle_app_id,
    .output_enter = handle_output_enter,
    .output_leave = handle_output_leave,
    .state = handle_state,
    .done = handle_done,
    .closed = handle_closed,
    .parent = handle_parent,
};

static void mgr_toplevel(void *data,
                          struct zwlr_foreign_toplevel_manager_v1 *mgr_proto,
                          struct zwlr_foreign_toplevel_handle_v1 *handle) {
    (void)mgr_proto;
    struct toplevel_manager *mgr = data;
    if (mgr->count >= MAX_TOPLEVELS) return;

    struct toplevel_info *tl = &mgr->toplevels[mgr->count];
    memset(tl, 0, sizeof(*tl));
    tl->handle = handle;
    tl->focus_seq = 0;
    mgr->count++;

    zwlr_foreign_toplevel_handle_v1_add_listener(handle, &handle_listener, mgr);
}

static void mgr_finished(void *data, struct zwlr_foreign_toplevel_manager_v1 *mgr_proto) {
    (void)mgr_proto;
    struct toplevel_manager *mgr = data;
    mgr->bar->toplevel_mgr_proto = NULL;
}

static const struct zwlr_foreign_toplevel_manager_v1_listener mgr_listener = {
    .toplevel = mgr_toplevel,
    .finished = mgr_finished,
};

void toplevel_manager_sort(struct toplevel_manager *mgr) {
    mgr->sorted_count = 0;
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->toplevels[i].output_count > 0)
            mgr->sorted_indices[mgr->sorted_count++] = i;
    }
}

struct toplevel_info *toplevel_at_x(struct toplevel_manager *mgr, double x) {
    for (int i = 0; i < mgr->sorted_count; i++) {
        struct toplevel_info *tl = &mgr->toplevels[mgr->sorted_indices[i]];
        if (x >= tl->render_x && x < tl->render_x + tl->render_w)
            return tl;
    }
    return NULL;
}

struct toplevel_manager *toplevel_manager_create(struct bar *bar) {
    struct toplevel_manager *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    mgr->bar = bar;
    mgr->icon_size = (bar->height - 8) * bar->scale;

    if (bar->toplevel_mgr_proto) {
        zwlr_foreign_toplevel_manager_v1_add_listener(
            bar->toplevel_mgr_proto, &mgr_listener, mgr);
    }

    return mgr;
}

void toplevel_manager_destroy(struct toplevel_manager *mgr) {
    if (!mgr) return;
    for (int i = 0; i < mgr->count; i++) {
        if (mgr->toplevels[i].icon)
            cairo_surface_destroy(mgr->toplevels[i].icon);
        if (mgr->toplevels[i].handle)
            zwlr_foreign_toplevel_handle_v1_destroy(mgr->toplevels[i].handle);
    }
    free(mgr);
}
