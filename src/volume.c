#include "volume.h"
#include "bar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <pipewire/pipewire.h>
#include <spa/param/param.h>

struct volume_node {
    struct volume *vol;
    struct pw_proxy *proxy;
    struct spa_hook proxy_listener;
    struct spa_hook node_listener;
    struct volume_node *next;
};

struct volume_pw {
    struct pw_main_loop *loop;
    struct pw_context *context;
    struct pw_core *core;
    struct pw_registry *registry;
    struct spa_hook registry_listener;
    struct volume_node *nodes;
    bool dirty;
};

static void on_node_param(void *data, int seq, uint32_t id,
                          uint32_t index, uint32_t next,
                          const struct spa_pod *param) {
    (void)seq; (void)id; (void)index; (void)next; (void)param;
    struct volume_node *np = data;
    np->vol->pw->dirty = true;
}

static const struct pw_node_events node_events = {
    PW_VERSION_NODE_EVENTS,
    .param = on_node_param,
};

static void on_proxy_removed(void *data) {
    struct volume_node *np = data;
    pw_proxy_destroy(np->proxy);
}

static void on_proxy_destroy(void *data) {
    struct volume_node *np = data;
    spa_hook_remove(&np->proxy_listener);
    spa_hook_remove(&np->node_listener);

    struct volume_node **pp = &np->vol->pw->nodes;
    while (*pp && *pp != np) pp = &(*pp)->next;
    if (*pp) *pp = np->next;

    np->vol->pw->dirty = true;
}

static const struct pw_proxy_events proxy_events = {
    PW_VERSION_PROXY_EVENTS,
    .removed = on_proxy_removed,
    .destroy = on_proxy_destroy,
};

static void on_registry_global(void *data, uint32_t id, uint32_t permissions,
                               const char *type, uint32_t version,
                               const struct spa_dict *props) {
    (void)id; (void)permissions; (void)version;
    struct volume *vol = data;
    if (strcmp(type, PW_TYPE_INTERFACE_Node) != 0) return;

    const char *media_class = props ? spa_dict_lookup(props, "media.class") : NULL;
    if (!media_class || strcmp(media_class, "Audio/Sink") != 0) return;

    struct pw_proxy *proxy = pw_registry_bind(vol->pw->registry, id,
        type, PW_VERSION_NODE, sizeof(struct volume_node));
    if (!proxy) return;

    struct volume_node *np = pw_proxy_get_user_data(proxy);
    memset(np, 0, sizeof(*np));
    np->vol = vol;
    np->proxy = proxy;
    np->next = vol->pw->nodes;
    vol->pw->nodes = np;

    pw_proxy_add_listener(proxy, &np->proxy_listener, &proxy_events, np);
    pw_proxy_add_object_listener(proxy, &np->node_listener, &node_events, np);

    uint32_t param_id = SPA_PARAM_Props;
    pw_node_subscribe_params((struct pw_node *)proxy, &param_id, 1);

    vol->pw->dirty = true;
}

static const struct pw_registry_events registry_events = {
    PW_VERSION_REGISTRY_EVENTS,
    .global = on_registry_global,
};

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

static void volume_pw_init(struct volume *vol) {
    pw_init(NULL, NULL);

    struct volume_pw *pw = calloc(1, sizeof(*pw));
    if (!pw) return;
    vol->pw = pw;

    pw->loop = pw_main_loop_new(NULL);
    if (!pw->loop) goto fail;

    pw->context = pw_context_new(pw_main_loop_get_loop(pw->loop), NULL, 0);
    if (!pw->context) goto fail;

    pw->core = pw_context_connect(pw->context, NULL, 0);
    if (!pw->core) {
        fprintf(stderr, "simpbar: PipeWire connect failed; falling back to polling\n");
        goto fail;
    }

    pw->registry = pw_core_get_registry(pw->core, PW_VERSION_REGISTRY, 0);
    if (!pw->registry) goto fail;

    pw_registry_add_listener(pw->registry, &pw->registry_listener,
                             &registry_events, vol);
    return;

fail:
    if (pw->core) pw_core_disconnect(pw->core);
    if (pw->context) pw_context_destroy(pw->context);
    if (pw->loop) pw_main_loop_destroy(pw->loop);
    free(pw);
    vol->pw = NULL;
}

struct volume *volume_create(struct bar *bar) {
    struct volume *vol = calloc(1, sizeof(*vol));
    if (!vol) return NULL;
    vol->bar = bar;
    vol->level = 50;
    vol->state = VOLUME_MEDIUM;

    volume_pw_init(vol);
    volume_update(vol);
    return vol;
}

void volume_destroy(struct volume *vol) {
    if (!vol) return;
    if (vol->pw) {
        while (vol->pw->nodes) {
            pw_proxy_destroy(vol->pw->nodes->proxy);
        }
        if (vol->pw->registry) pw_proxy_destroy((struct pw_proxy *)vol->pw->registry);
        if (vol->pw->core) pw_core_disconnect(vol->pw->core);
        if (vol->pw->context) pw_context_destroy(vol->pw->context);
        if (vol->pw->loop) pw_main_loop_destroy(vol->pw->loop);
        free(vol->pw);
        pw_deinit();
    }
    free(vol);
}

int volume_get_fd(struct volume *vol) {
    if (!vol || !vol->pw || !vol->pw->loop) return -1;
    return pw_loop_get_fd(pw_main_loop_get_loop(vol->pw->loop));
}

bool volume_dispatch(struct volume *vol) {
    if (!vol || !vol->pw || !vol->pw->loop) return false;
    struct pw_loop *loop = pw_main_loop_get_loop(vol->pw->loop);
    pw_loop_enter(loop);
    pw_loop_iterate(loop, 0);
    pw_loop_leave(loop);

    if (vol->pw->dirty) {
        vol->pw->dirty = false;
        int prev_level = vol->level;
        bool prev_muted = vol->muted;
        volume_update(vol);
        return vol->level != prev_level || vol->muted != prev_muted;
    }
    return false;
}

void volume_render(struct volume *vol) { (void)vol; }
