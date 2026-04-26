#ifndef VOLUME_H
#define VOLUME_H

#include <stdbool.h>

struct bar;
struct volume_pw;

enum volume_state {
    VOLUME_MUTED,
    VOLUME_LOW,
    VOLUME_MEDIUM,
    VOLUME_HIGH,
};

struct volume {
    struct bar *bar;
    int level;        /* 0-100 */
    enum volume_state state;
    bool muted;

    struct volume_pw *pw;  /* opaque PipeWire state */
};

struct volume *volume_create(struct bar *bar);
void volume_destroy(struct volume *vol);
void volume_update(struct volume *vol);
void volume_render(struct volume *vol);

/* PipeWire event-loop integration. Returns -1 if PipeWire is unavailable. */
int volume_get_fd(struct volume *vol);
/* Iterate the PipeWire loop once. Returns true if volume state changed and
 * the bar should be redrawn. */
bool volume_dispatch(struct volume *vol);

#endif /* VOLUME_H */
