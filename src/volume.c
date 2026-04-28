#include "volume.h"
#include "bar.h"

#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include <pipewire/pipewire.h>
#include <spa/param/param.h>

/* How long the transient volume pill stays on screen after the last change. */
#define VOLUME_VISIBLE_MS 1500

/* Reconnect backoff: start at 500ms, double on each failure, cap at 30s. */
#define RECONNECT_INITIAL_MS 500
#define RECONNECT_MAX_MS     30000

static int64_t monotonic_ms(void) {
    struct timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return (int64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
}

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
    struct spa_hook core_listener;
    struct spa_hook registry_listener;
    struct volume_node *nodes;
    struct spa_source *retry_timer;
    int retry_delay_ms;
    bool disconnected;
    bool dirty;
};

static int volume_pw_connect(struct volume *vol);
static void volume_pw_teardown(struct volume *vol);
static void volume_pw_schedule_retry(struct volume *vol);

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

static void on_core_error(void *data, uint32_t id, int seq, int res,
                          const char *message) {
    (void)seq;
    struct volume *vol = data;
    /* id == PW_ID_CORE with -EPIPE is the canonical "daemon went away" signal. */
    if (id == PW_ID_CORE && res == -EPIPE) {
        vol->pw->disconnected = true;
    } else {
        fprintf(stderr, "simpbar: pipewire error id:%u res:%d: %s\n",
                id, res, message ? message : "");
    }
}

static const struct pw_core_events core_events = {
    PW_VERSION_CORE_EVENTS,
    .error = on_core_error,
};

static void on_retry_timer(void *data, uint64_t expirations) {
    (void)expirations;
    struct volume *vol = data;
    struct volume_pw *pw = vol->pw;

    /* Disarm; we'll re-arm only if the connect attempt fails. */
    struct timespec zero = {0, 0};
    pw_loop_update_timer(pw_main_loop_get_loop(pw->loop),
                         pw->retry_timer, &zero, &zero, false);

    if (volume_pw_connect(vol) == 0) {
        pw->retry_delay_ms = RECONNECT_INITIAL_MS;
        /* Force a refresh — we likely missed events while disconnected. */
        pw->dirty = true;
    } else {
        volume_pw_schedule_retry(vol);
    }
}

static void volume_pw_schedule_retry(struct volume *vol) {
    struct volume_pw *pw = vol->pw;
    int delay = pw->retry_delay_ms;
    pw->retry_delay_ms *= 2;
    if (pw->retry_delay_ms > RECONNECT_MAX_MS) pw->retry_delay_ms = RECONNECT_MAX_MS;

    struct timespec value = {
        .tv_sec  = delay / 1000,
        .tv_nsec = (delay % 1000) * 1000000L,
    };
    struct timespec interval = {0, 0};
    pw_loop_update_timer(pw_main_loop_get_loop(pw->loop),
                         pw->retry_timer, &value, &interval, false);
}

static int volume_pw_connect(struct volume *vol) {
    struct volume_pw *pw = vol->pw;

    pw->context = pw_context_new(pw_main_loop_get_loop(pw->loop), NULL, 0);
    if (!pw->context) return -1;

    pw->core = pw_context_connect(pw->context, NULL, 0);
    if (!pw->core) {
        pw_context_destroy(pw->context);
        pw->context = NULL;
        return -1;
    }
    pw_core_add_listener(pw->core, &pw->core_listener, &core_events, vol);

    pw->registry = pw_core_get_registry(pw->core, PW_VERSION_REGISTRY, 0);
    if (!pw->registry) {
        spa_hook_remove(&pw->core_listener);
        pw_core_disconnect(pw->core);
        pw_context_destroy(pw->context);
        pw->core = NULL;
        pw->context = NULL;
        return -1;
    }
    pw_registry_add_listener(pw->registry, &pw->registry_listener,
                             &registry_events, vol);

    pw->disconnected = false;
    return 0;
}

static void volume_pw_teardown(struct volume *vol) {
    struct volume_pw *pw = vol->pw;
    while (pw->nodes) {
        pw_proxy_destroy(pw->nodes->proxy);
    }
    if (pw->registry) {
        pw_proxy_destroy((struct pw_proxy *)pw->registry);
        pw->registry = NULL;
    }
    if (pw->core) {
        spa_hook_remove(&pw->core_listener);
        pw_core_disconnect(pw->core);
        pw->core = NULL;
    }
    if (pw->context) {
        pw_context_destroy(pw->context);
        pw->context = NULL;
    }
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

static void volume_pw_init(struct volume *vol) {
    pw_init(NULL, NULL);

    struct volume_pw *pw = calloc(1, sizeof(*pw));
    if (!pw) return;
    vol->pw = pw;
    pw->retry_delay_ms = RECONNECT_INITIAL_MS;

    pw->loop = pw_main_loop_new(NULL);
    if (!pw->loop) {
        free(pw);
        vol->pw = NULL;
        return;
    }

    pw->retry_timer = pw_loop_add_timer(pw_main_loop_get_loop(pw->loop),
                                        on_retry_timer, vol);
    if (!pw->retry_timer) {
        pw_main_loop_destroy(pw->loop);
        free(pw);
        vol->pw = NULL;
        return;
    }

    if (volume_pw_connect(vol) != 0) {
        fprintf(stderr, "simpbar: PipeWire connect failed; will retry\n");
        volume_pw_schedule_retry(vol);
    }
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
        volume_pw_teardown(vol);
        if (vol->pw->retry_timer) {
            pw_loop_destroy_source(pw_main_loop_get_loop(vol->pw->loop),
                                   vol->pw->retry_timer);
        }
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

    /* The error callback can't safely tear down the connection from inside
     * iterate, so it just sets a flag. Handle the teardown + retry here. */
    if (vol->pw->disconnected) {
        vol->pw->disconnected = false;
        volume_pw_teardown(vol);
        volume_pw_schedule_retry(vol);
    }

    if (vol->pw->dirty) {
        vol->pw->dirty = false;
        int prev_level = vol->level;
        bool prev_muted = vol->muted;
        volume_update(vol);
        bool changed = vol->level != prev_level || vol->muted != prev_muted;
        if (changed) {
            vol->visible_until_ms = monotonic_ms() + VOLUME_VISIBLE_MS;
        }
        return changed;
    }
    return false;
}

bool volume_visible(const struct volume *vol) {
    if (!vol) return false;
    return vol->visible_until_ms > monotonic_ms();
}

int volume_remaining_ms(const struct volume *vol) {
    if (!vol) return -1;
    int64_t remaining = vol->visible_until_ms - monotonic_ms();
    if (remaining <= 0) return -1;
    return (int)remaining;
}

void volume_render(struct volume *vol) { (void)vol; }
