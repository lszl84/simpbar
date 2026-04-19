#include "icon_loader.h"

#include <stdbool.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <librsvg/rsvg.h>

static bool find_desktop_icon(const char *dir, const char *app_id,
                              char *icon_out, size_t icon_sz) {
    char path[1024];

    snprintf(path, sizeof(path), "%s/%s.desktop", dir, app_id);
    FILE *f = fopen(path, "r");

    if (!f) {
        char lower[128] = {0};
        for (int i = 0; app_id[i] && i < 127; i++)
            lower[i] = (app_id[i] >= 'A' && app_id[i] <= 'Z')
                ? app_id[i] + 32 : app_id[i];
        snprintf(path, sizeof(path), "%s/%s.desktop", dir, lower);
        f = fopen(path, "r");
    }

    if (!f) {
        DIR *dp = opendir(dir);
        if (!dp) return false;
        struct dirent *ent;
        while ((ent = readdir(dp))) {
            if (!strstr(ent->d_name, ".desktop")) continue;
            char base[256];
            char *dot = strrchr(ent->d_name, '.');
            if (!dot) continue;
            int len = dot - ent->d_name;
            if (len >= (int)sizeof(base)) len = sizeof(base) - 1;
            memcpy(base, ent->d_name, len);
            base[len] = '\0';

            char *last = strrchr(base, '.');
            const char *short_name = last ? last + 1 : base;
            if (strcasecmp(short_name, app_id) == 0) {
                snprintf(path, sizeof(path), "%s/%s", dir, ent->d_name);
                f = fopen(path, "r");
                if (f) break;
            }
        }
        closedir(dp);
    }

    if (!f) return false;

    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        if (strncmp(line, "Icon=", 5) == 0) {
            line[strcspn(line, "\r\n")] = '\0';
            strncpy(icon_out, line + 5, icon_sz - 1);
            icon_out[icon_sz - 1] = '\0';
            fclose(f);
            return true;
        }
    }
    fclose(f);
    return false;
}

static bool find_icon_name(const char *app_id, char *icon_out, size_t icon_sz) {
    const char *home = getenv("HOME");
    if (home) {
        char dir[512];
        snprintf(dir, sizeof(dir), "%s/.local/share/applications", home);
        if (find_desktop_icon(dir, app_id, icon_out, icon_sz)) return true;
    }

    static const char *sys_dirs[] = {
        "/usr/share/applications",
        "/usr/local/share/applications",
        NULL
    };
    for (int i = 0; sys_dirs[i]; i++) {
        if (find_desktop_icon(sys_dirs[i], app_id, icon_out, icon_sz))
            return true;
    }

    strncpy(icon_out, app_id, icon_sz - 1);
    icon_out[icon_sz - 1] = '\0';
    return false;
}

static cairo_surface_t *load_svg(const char *path, int size) {
    GError *error = NULL;
    RsvgHandle *handle = rsvg_handle_new_from_file(path, &error);
    if (!handle) {
        if (error) g_error_free(error);
        return NULL;
    }

    cairo_surface_t *surface = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(surface);

    RsvgRectangle viewport = {0, 0, size, size};
    rsvg_handle_render_document(handle, cr, &viewport, NULL);

    cairo_destroy(cr);
    g_object_unref(handle);
    return surface;
}

static cairo_surface_t *load_png(const char *path, int size) {
    cairo_surface_t *raw = cairo_image_surface_create_from_png(path);
    if (cairo_surface_status(raw) != CAIRO_STATUS_SUCCESS) {
        cairo_surface_destroy(raw);
        return NULL;
    }

    int w = cairo_image_surface_get_width(raw);
    int h = cairo_image_surface_get_height(raw);
    if (w == size && h == size) return raw;

    cairo_surface_t *scaled = cairo_image_surface_create(
        CAIRO_FORMAT_ARGB32, size, size);
    cairo_t *cr = cairo_create(scaled);
    double s = (double)size / (w > h ? w : h);
    double ox = (size - w * s) / 2.0;
    double oy = (size - h * s) / 2.0;
    cairo_translate(cr, ox, oy);
    cairo_scale(cr, s, s);
    cairo_set_source_surface(cr, raw, 0, 0);
    cairo_pattern_set_filter(cairo_get_source(cr), CAIRO_FILTER_BILINEAR);
    cairo_paint(cr);
    cairo_destroy(cr);
    cairo_surface_destroy(raw);
    return scaled;
}

static cairo_surface_t *try_file(const char *path, int size) {
    if (access(path, R_OK) != 0) return NULL;
    const char *ext = strrchr(path, '.');
    if (ext && strcasecmp(ext, ".svg") == 0)
        return load_svg(path, size);
    return load_png(path, size);
}

static cairo_surface_t *resolve_icon(const char *name, int size) {
    if (!name || !name[0]) return NULL;

    if (name[0] == '/')
        return try_file(name, size);

    char path[1024];
    static const char *themes[] = {"hicolor", "Adwaita", NULL};
    const char *home = getenv("HOME");

    const char *base_dirs[3];
    int n_dirs = 0;
    char home_icons[512] = {0};
    if (home) {
        snprintf(home_icons, sizeof(home_icons), "%s/.local/share/icons", home);
        base_dirs[n_dirs++] = home_icons;
    }
    base_dirs[n_dirs++] = "/usr/share/icons";
    base_dirs[n_dirs] = NULL;

    for (int d = 0; d < n_dirs; d++) {
        for (int t = 0; themes[t]; t++) {
            snprintf(path, sizeof(path),
                "%s/%s/scalable/apps/%s.svg", base_dirs[d], themes[t], name);
            cairo_surface_t *s = try_file(path, size);
            if (s) return s;
        }
    }

    static const char *px_sizes[] = {
        "256x256", "128x128", "64x64", "48x48", "32x32", NULL
    };
    for (int d = 0; d < n_dirs; d++) {
        for (int t = 0; themes[t]; t++) {
            for (int i = 0; px_sizes[i]; i++) {
                snprintf(path, sizeof(path),
                    "%s/%s/%s/apps/%s.png",
                    base_dirs[d], themes[t], px_sizes[i], name);
                cairo_surface_t *s = try_file(path, size);
                if (s) return s;
            }
        }
    }

    const char *suffixes[] = {".svg", ".png", NULL};
    for (int i = 0; suffixes[i]; i++) {
        snprintf(path, sizeof(path), "/usr/share/pixmaps/%s%s", name, suffixes[i]);
        cairo_surface_t *s = try_file(path, size);
        if (s) return s;
    }

    return NULL;
}

cairo_surface_t *icon_load(const char *app_id, int size) {
    if (!app_id || !app_id[0]) return NULL;

    char icon_name[256];
    find_icon_name(app_id, icon_name, sizeof(icon_name));

    cairo_surface_t *s = resolve_icon(icon_name, size);
    if (s) return s;

    if (strcmp(icon_name, app_id) != 0) {
        s = resolve_icon(app_id, size);
        if (s) return s;
    }

    char lower[256] = {0};
    for (int i = 0; icon_name[i] && i < 255; i++)
        lower[i] = (icon_name[i] >= 'A' && icon_name[i] <= 'Z')
            ? icon_name[i] + 32 : icon_name[i];
    if (strcmp(lower, icon_name) != 0) {
        s = resolve_icon(lower, size);
        if (s) return s;
    }

    return NULL;
}
