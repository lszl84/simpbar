#include "workspace.h"
#include "bar.h"
#include "ext-workspace-v1-client-protocol.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static struct workspace_info *find_workspace_by_handle(
    struct workspace_manager *mgr, struct ext_workspace_handle_v1 *handle) {
    for (int i = 0; i < mgr->workspace_count; i++) {
        if (mgr->workspaces[i].handle == handle) return &mgr->workspaces[i];
    }
    return NULL;
}

static void ws_handle_id(void *data, struct ext_workspace_handle_v1 *handle,
                         const char *id) {
    (void)data; (void)handle; (void)id;
}

static void ws_handle_name(void *data, struct ext_workspace_handle_v1 *handle,
                           const char *name) {
    struct workspace_manager *mgr = data;
    struct workspace_info *ws = find_workspace_by_handle(mgr, handle);
    if (ws) {
        strncpy(ws->name, name, sizeof(ws->name) - 1);
    }
}

static void ws_handle_coordinates(void *data, struct ext_workspace_handle_v1 *handle,
                                  struct wl_array *coords) {
    (void)data; (void)handle; (void)coords;
}

static void ws_handle_state(void *data, struct ext_workspace_handle_v1 *handle,
                            uint32_t state) {
    struct workspace_manager *mgr = data;
    struct workspace_info *ws = find_workspace_by_handle(mgr, handle);
    if (ws) {
        ws->state = (state & EXT_WORKSPACE_HANDLE_V1_STATE_ACTIVE)
                        ? WORKSPACE_ACTIVE : WORKSPACE_INACTIVE;
        mgr->dirty = true;
    }
}

static void ws_handle_capabilities(void *data, struct ext_workspace_handle_v1 *handle,
                                   uint32_t caps) {
    (void)data; (void)handle; (void)caps;
}

static void ws_handle_removed(void *data, struct ext_workspace_handle_v1 *handle) {
    struct workspace_manager *mgr = data;
    for (int i = 0; i < mgr->workspace_count; i++) {
        if (mgr->workspaces[i].handle == handle) {
            ext_workspace_handle_v1_destroy(handle);
            for (int j = i; j < mgr->workspace_count - 1; j++) {
                mgr->workspaces[j] = mgr->workspaces[j + 1];
            }
            mgr->workspace_count--;
            mgr->dirty = true;
            return;
        }
    }
}

static const struct ext_workspace_handle_v1_listener ws_handle_listener = {
    .id = ws_handle_id,
    .name = ws_handle_name,
    .coordinates = ws_handle_coordinates,
    .state = ws_handle_state,
    .capabilities = ws_handle_capabilities,
    .removed = ws_handle_removed,
};

static void mgr_workspace_group(void *data,
                                struct ext_workspace_manager_v1 *mgr_proto,
                                struct ext_workspace_group_handle_v1 *group) {
    (void)data; (void)mgr_proto; (void)group;
}

static void mgr_workspace(void *data,
                           struct ext_workspace_manager_v1 *mgr_proto,
                           struct ext_workspace_handle_v1 *handle) {
    (void)mgr_proto;
    struct workspace_manager *mgr = data;
    if (mgr->workspace_count >= MAX_WORKSPACES) return;

    struct workspace_info *ws = &mgr->workspaces[mgr->workspace_count];
    memset(ws, 0, sizeof(*ws));
    ws->handle = handle;
    ws->state = WORKSPACE_INACTIVE;
    ws->index = mgr->workspace_count;
    mgr->workspace_count++;

    ext_workspace_handle_v1_add_listener(handle, &ws_handle_listener, mgr);
}

static void mgr_done(void *data, struct ext_workspace_manager_v1 *mgr_proto) {
    (void)mgr_proto;
    struct workspace_manager *mgr = data;
    if (mgr->dirty) {
        mgr->dirty = false;
        bar_redraw(mgr->bar);
    }
}

static void mgr_finished(void *data, struct ext_workspace_manager_v1 *mgr_proto) {
    (void)mgr_proto;
    struct workspace_manager *mgr = data;
    mgr->bar->workspace_proto = NULL;
}

static const struct ext_workspace_manager_v1_listener mgr_listener = {
    .workspace_group = mgr_workspace_group,
    .workspace = mgr_workspace,
    .done = mgr_done,
    .finished = mgr_finished,
};

struct workspace_manager *workspace_manager_create(struct bar *bar) {
    struct workspace_manager *mgr = calloc(1, sizeof(*mgr));
    if (!mgr) return NULL;
    mgr->bar = bar;

    if (bar->workspace_proto) {
        ext_workspace_manager_v1_add_listener(bar->workspace_proto, &mgr_listener, mgr);
        wl_display_roundtrip(bar->display);
    } else {
        mgr->workspace_count = 4;
        mgr->workspaces[0].state = WORKSPACE_ACTIVE;
        for (int i = 1; i < 4; i++)
            mgr->workspaces[i].state = WORKSPACE_INACTIVE;
    }

    return mgr;
}

void workspace_manager_destroy(struct workspace_manager *mgr) {
    if (!mgr) return;
    for (int i = 0; i < mgr->workspace_count; i++) {
        if (mgr->workspaces[i].handle)
            ext_workspace_handle_v1_destroy(mgr->workspaces[i].handle);
    }
    free(mgr);
}
