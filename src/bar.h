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

/* Layer surface lifecycle — invoked by registry when our wl_output goes
 * away (e.g. across a VT switch) and a new one appears. */
void bar_setup_layer_surface(struct bar *bar);
void bar_teardown_layer_surface(struct bar *bar);

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
    uint32_t output_id;
    struct ext_workspace_manager_v1 *workspace_proto;
    struct zwlr_foreign_toplevel_manager_v1 *toplevel_mgr_proto;
    struct xdg_wm_base *xdg_wm_base;

    struct workspace_manager *workspace_mgr;
    struct toplevel_manager *toplevel_mgr;
    struct battery *battery;
    struct battery_history *battery_history;
    struct volume *volume;
    struct backlight *backlight;
    struct clock *clock;
    struct popup *battery_popup;

    int width;
    int height;
    int scale;
    bool configured;
    bool pending_redraw;
    bool initialized;  /* bar_create completed; safe to handle layer events */
    int running;

    double pointer_x;
    double pointer_y;
    bool pointer_inside;

    /* Battery icon hit rect (in surface coords) */
    double battery_hit_x;
    double battery_hit_y;
    double battery_hit_w;
    double battery_hit_h;

    bool pointer_over_battery;
    bool pointer_over_popup;
};

#endif
