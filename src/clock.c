#include "clock.h"
#include "bar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <sys/time.h>

struct clock *clock_create(struct bar *bar) {
    struct clock *clk = calloc(1, sizeof(struct clock));
    if (!clk) return NULL;

    clk->bar = bar;
    clk->last_update = 0;

    clock_update(clk);

    return clk;
}

void clock_destroy(struct clock *clk) {
    free(clk);
}

void clock_update(struct clock *clk) {
    time_t now = time(NULL);
    
    if (now == clk->last_update) return;

    struct tm *tm_info = localtime(&now);

    strftime(clk->time_str, sizeof(clk->time_str), "%H:%M", tm_info);
    strftime(clk->date_str, sizeof(clk->date_str), "%a %b %d", tm_info);

    clk->last_update = now;
}

void clock_render(struct clock *clk) {
    (void)clk;
    // Rendering is handled in gl-renderer.c
}