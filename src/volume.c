#include "volume.h"
#include "bar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct volume *volume_create(struct bar *bar) {
    struct volume *vol = calloc(1, sizeof(*vol));
    if (!vol) return NULL;
    vol->bar = bar;
    vol->level = 50;
    vol->state = VOLUME_MEDIUM;
    volume_update(vol);
    return vol;
}

void volume_destroy(struct volume *vol) {
    free(vol);
}

void volume_update(struct volume *vol) {
    FILE *fp = popen("wpctl get-volume @DEFAULT_AUDIO_SINK@ 2>/dev/null", "r");
    if (fp) {
        char line[128];
        if (fgets(line, sizeof(line), fp)) {
            float fvol = 0;
            if (sscanf(line, "Volume: %f", &fvol) == 1) {
                vol->level = (int)(fvol * 100 + 0.5f);
                if (vol->level > 100) vol->level = 100;
                if (vol->level < 0) vol->level = 0;
            }
            vol->muted = (strstr(line, "[MUTED]") != NULL);
        }
        pclose(fp);
    }

    if (vol->muted) vol->state = VOLUME_MUTED;
    else if (vol->level > 66) vol->state = VOLUME_HIGH;
    else if (vol->level > 33) vol->state = VOLUME_MEDIUM;
    else vol->state = VOLUME_LOW;
}

void volume_render(struct volume *vol) { (void)vol; }
