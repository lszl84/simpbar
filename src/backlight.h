#ifndef BACKLIGHT_H
#define BACKLIGHT_H

#include <stdbool.h>
#include <stdint.h>

struct bar;
struct udev;
struct udev_monitor;

#define MAX_BACKLIGHT_PATH 256

struct backlight {
    struct bar *bar;
    bool available;
    char path[MAX_BACKLIGHT_PATH];
    int current;
    int max;

    struct udev *udev;
    struct udev_monitor *monitor;
    int monitor_fd;

    /* Monotonic deadline (ms) until the indicator should be visible.
     * 0 = never shown; past = hidden. Set on each detected change. */
    int64_t visible_until_ms;
};

struct backlight *backlight_create(struct bar *bar);
void backlight_destroy(struct backlight *bl);
void backlight_update(struct backlight *bl);
int backlight_get_fd(struct backlight *bl);
bool backlight_dispatch(struct backlight *bl);
int backlight_percent(const struct backlight *bl);
/* 0.0..1.0 fill, gamma-corrected so the bar tracks perceived brightness
 * rather than the raw linear PWM value. */
double backlight_perceptual_fraction(const struct backlight *bl);

/* Transient indicator visibility. */
bool backlight_visible(const struct backlight *bl);
/* ms remaining until the indicator hides, or -1 if not currently visible. */
int  backlight_remaining_ms(const struct backlight *bl);

#endif
