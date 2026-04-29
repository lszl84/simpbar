#ifndef BATTERY_H
#define BATTERY_H

#include <stdbool.h>

#define MAX_BATTERY_PATH 256

struct bar;

enum battery_state {
    BATTERY_CHARGING,
    BATTERY_DISCHARGING,
    BATTERY_FULL,
    BATTERY_UNKNOWN,
};

struct battery {
    struct bar *bar;
    char path[MAX_BATTERY_PATH];
    int percentage;
    enum battery_state state;
    bool available;
    int time_remaining;  // minutes, -1 if unknown
};

struct battery *battery_create(struct bar *bar);
void battery_destroy(struct battery *bat);
void battery_update(struct battery *bat);
void battery_render(struct battery *bat);

#endif /* BATTERY_H */