#ifndef SIMPBAR_POPUP_H
#define SIMPBAR_POPUP_H

#include <stdbool.h>
#include <wayland-client.h>

struct bar;
struct popup;

struct popup *popup_create_battery(struct bar *bar,
                                   double anchor_x, double anchor_y,
                                   double anchor_w, double anchor_h);
void popup_destroy(struct popup *p);
void popup_redraw(struct popup *p);

/* Returns true if the given wl_surface is the popup's surface. */
bool popup_owns_surface(struct popup *p, struct wl_surface *surface);

#endif
