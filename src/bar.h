#ifndef BAR_H
#define BAR_H

#include <stdbool.h>
#include <stdint.h>
#include <wayland-client.h>

struct bar;

struct bar *bar_create(void);
void bar_destroy(struct bar *bar);
int bar_run(struct bar *bar);
void bar_redraw(struct bar *bar);

struct bar {
    struct wl_display *display;
    struct wl_registry *registry;
    struct wl_compositor *compositor;
    struct wl_subcompositor *subcompositor;
    struct wl_shm *shm;
    struct wl_seat *seat;
    struct wl_pointer *pointer;
    struct wl_cursor_theme *cursor_theme;
    struct wl_surface *cursor_surface;
    struct zwlr_layer_shell_v1 *layer_shell;
    struct zwlr_layer_surface_v1 *layer_surface;
    struct wl_surface *surface;
    struct wl_output *output;
    struct ext_workspace_manager_v1 *workspace_proto;
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_mgr_proto;

    struct workspace_manager *workspace_mgr;
    struct toplevel_manager *toplevel_mgr;
    struct battery *battery;
    struct volume *volume;
    struct clock *clock;

    int width;
    int height;
    int scale;
    bool configured;
    int running;

    double pointer_x;
    double pointer_y;
    bool pointer_inside;
};

#endif
