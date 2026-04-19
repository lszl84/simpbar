#ifndef CLOCK_H
#define CLOCK_H

#include <stdbool.h>
#include <time.h>

struct bar;

struct clock {
    struct bar *bar;
    time_t last_update;
    char time_str[64];
    char date_str[64];
};

struct clock *clock_create(struct bar *bar);
void clock_destroy(struct clock *clk);
void clock_update(struct clock *clk);
void clock_render(struct clock *clk);

#endif /* CLOCK_H */