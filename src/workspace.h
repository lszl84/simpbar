#ifndef WORKSPACE_H
#define WORKSPACE_H

#include <stdbool.h>

#define MAX_WORKSPACES 10

struct bar;

enum workspace_state {
    WORKSPACE_INACTIVE,
    WORKSPACE_ACTIVE,
};

struct workspace_info {
    struct ext_workspace_handle_v1 *handle;
    enum workspace_state state;
    char name[32];
    int index;
};

struct workspace_manager {
    struct bar *bar;
    int workspace_count;
    int active_workspace;
    struct workspace_info workspaces[MAX_WORKSPACES];
    bool dirty;
};

struct workspace_manager *workspace_manager_create(struct bar *bar);
void workspace_manager_destroy(struct workspace_manager *mgr);

#endif
