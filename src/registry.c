#include "registry.h"
#include "bar.h"
#include "wlr-layer-shell-unstable-v1-client-protocol.h"
#include "ext-workspace-v1-client-protocol.h"

#include <stdio.h>
#include <string.h>

static void registry_handle_global(void *data, struct wl_registry *registry,
                                   uint32_t id, const char *interface, uint32_t version) {
    struct bar *bar = data;

    if (strcmp(interface, "wl_compositor") == 0) {
        bar->compositor = wl_registry_bind(registry, id, &wl_compositor_interface, 4);
    } else if (strcmp(interface, "wl_subcompositor") == 0) {
        bar->subcompositor = wl_registry_bind(registry, id, &wl_subcompositor_interface, 1);
    } else if (strcmp(interface, "wl_shm") == 0) {
        bar->shm = wl_registry_bind(registry, id, &wl_shm_interface, 1);
    } else if (strcmp(interface, "zwlr_layer_shell_v1") == 0) {
        bar->layer_shell = wl_registry_bind(registry, id, &zwlr_layer_shell_v1_interface,
                                            version > 4 ? 4 : version);
    } else if (strcmp(interface, "wl_output") == 0) {
        if (!bar->output) {
            bar->output = wl_registry_bind(registry, id, &wl_output_interface, 1);
        }
    } else if (strcmp(interface, "ext_workspace_manager_v1") == 0) {
        bar->workspace_proto = wl_registry_bind(registry, id,
                                                &ext_workspace_manager_v1_interface, 1);
    }
}

static void registry_handle_global_remove(void *data, struct wl_registry *registry, uint32_t id) {
    (void)data; (void)registry; (void)id;
}

static const struct wl_registry_listener registry_listener = {
    .global = registry_handle_global,
    .global_remove = registry_handle_global_remove,
};

void registry_init(struct bar *bar) {
    bar->registry = wl_display_get_registry(bar->display);
    wl_registry_add_listener(bar->registry, &registry_listener, bar);
    wl_display_roundtrip(bar->display);
}

void registry_fini(struct bar *bar) {
    if (bar->registry) {
        wl_registry_destroy(bar->registry);
        bar->registry = NULL;
    }
}
