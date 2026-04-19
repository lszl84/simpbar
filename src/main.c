#include "bar.h"

#include <stdio.h>
#include <signal.h>

static struct bar *global_bar = NULL;

static void signal_handler(int sig) {
    (void)sig;
    if (global_bar) global_bar->running = 0;
}

int main(void) {
    signal(SIGINT, signal_handler);
    signal(SIGTERM, signal_handler);

    struct bar *bar = bar_create();
    if (!bar) {
        fprintf(stderr, "Failed to create bar\n");
        return 1;
    }

    global_bar = bar;
    int ret = bar_run(bar);
    bar_destroy(bar);
    return ret;
}
