#include "backlight.h"
#include "bar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <math.h>
#include <time.h>
#include <libudev.h>

/* How long the brightness indicator stays on screen after the last change. */
#define BACKLIGHT_VISIBLE_MS 1500

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

static int read_int_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    int v = -1;
    if (fscanf(f, "%d", &v) != 1) v = -1;
    fclose(f);
    return v;
}

static bool find_backlight(char *out, size_t out_sz) {
    DIR *d = opendir("/sys/class/backlight");
    if (!d) return false;
    struct dirent *e;
    char *first = NULL;
    char first_buf[MAX_BACKLIGHT_PATH] = {0};
    while ((e = readdir(d))) {
        if (e->d_name[0] == '.') continue;
        /* Prefer firmware/raw backlight (has max_brightness file). */
        snprintf(first_buf, sizeof(first_buf),
                 "/sys/class/backlight/%s", e->d_name);
        first = first_buf;
        break;
    }
    closedir(d);
    if (!first) return false;
    snprintf(out, out_sz, "%s", first);
    return true;
}

void backlight_update(struct backlight *bl) {
    if (!bl->available) return;
    char buf[MAX_BACKLIGHT_PATH + 32];
    snprintf(buf, sizeof(buf), "%s/actual_brightness", bl->path);
    int v = read_int_file(buf);
    if (v >= 0) bl->current = v;
}

struct backlight *backlight_create(struct bar *bar) {
    struct backlight *bl = calloc(1, sizeof(*bl));
    if (!bl) return NULL;
    bl->bar = bar;
    bl->monitor_fd = -1;

    if (!find_backlight(bl->path, sizeof(bl->path))) return bl;

    char buf[MAX_BACKLIGHT_PATH + 32];
    snprintf(buf, sizeof(buf), "%s/max_brightness", bl->path);
    int max = read_int_file(buf);
    if (max <= 0) return bl;
    bl->max = max;
    bl->available = true;
    backlight_update(bl);

    bl->udev = udev_new();
    if (!bl->udev) return bl;
    bl->monitor = udev_monitor_new_from_netlink(bl->udev, "udev");
    if (!bl->monitor) return bl;
    udev_monitor_filter_add_match_subsystem_devtype(bl->monitor, "backlight", NULL);
    udev_monitor_enable_receiving(bl->monitor);
    bl->monitor_fd = udev_monitor_get_fd(bl->monitor);
    return bl;
}

void backlight_destroy(struct backlight *bl) {
    if (!bl) return;
    if (bl->monitor) udev_monitor_unref(bl->monitor);
    if (bl->udev) udev_unref(bl->udev);
    free(bl);
}

int backlight_get_fd(struct backlight *bl) {
    if (!bl) return -1;
    return bl->monitor_fd;
}

bool backlight_dispatch(struct backlight *bl) {
    if (!bl || !bl->monitor) return false;
    bool changed = false;
    struct udev_device *dev;
    /* Drain all pending events. */
    while ((dev = udev_monitor_receive_device(bl->monitor)) != NULL) {
        int prev = bl->current;
        backlight_update(bl);
        if (bl->current != prev) changed = true;
        udev_device_unref(dev);
    }
    if (changed) {
        bl->visible_until_ms = monotonic_ms() + BACKLIGHT_VISIBLE_MS;
    }
    return changed;
}

int backlight_percent(const struct backlight *bl) {
    if (!bl || !bl->available || bl->max <= 0) return 0;
    int pct = (int)((bl->current * 100 + bl->max / 2) / bl->max);
    if (pct < 0) pct = 0;
    if (pct > 100) pct = 100;
    return pct;
}

double backlight_perceptual_fraction(const struct backlight *bl) {
    if (!bl || !bl->available || bl->max <= 0) return 0.0;
    double f = (double)bl->current / (double)bl->max;
    if (f < 0.0) f = 0.0;
    if (f > 1.0) f = 1.0;
    /* Backlight raw value is roughly linear in luminance; perceived
     * brightness follows ~luminance^(1/2.2). */
    return pow(f, 1.0 / 2.2);
}

bool backlight_visible(const struct backlight *bl) {
    if (!bl || !bl->available) return false;
    return bl->visible_until_ms > monotonic_ms();
}

int backlight_remaining_ms(const struct backlight *bl) {
    if (!bl || !bl->available) return -1;
    int64_t remaining = bl->visible_until_ms - monotonic_ms();
    if (remaining <= 0) return -1;
    return (int)remaining;
}
