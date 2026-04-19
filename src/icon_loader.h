#ifndef ICON_LOADER_H
#define ICON_LOADER_H

#include <cairo/cairo.h>

cairo_surface_t *icon_load(const char *app_id, int size);

#endif
