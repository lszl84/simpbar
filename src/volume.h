#ifndef VOLUME_H
#define VOLUME_H

#include <stdbool.h>

struct bar;

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
};

struct volume *volume_create(struct bar *bar);
void volume_destroy(struct volume *vol);
void volume_update(struct volume *vol);
void volume_render(struct volume *vol);

#endif /* VOLUME_H */