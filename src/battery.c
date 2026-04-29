#include "battery.h"
#include "bar.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <dirent.h>
#include <fcntl.h>
#include <unistd.h>
#include <ctype.h>

static char *find_battery_path(void) {
    static char path[256];
    DIR *dir = opendir("/sys/class/power_supply");
    if (!dir) return NULL;

    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL) {
        if (strncmp(entry->d_name, "BAT", 3) == 0) {
            snprintf(path, sizeof(path), "/sys/class/power_supply/%s", entry->d_name);
            closedir(dir);
            return path;
        }
    }

    closedir(dir);
    return NULL;
}

static int read_int_from_file(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;

    int value;
    if (fscanf(f, "%d", &value) != 1) {
        fclose(f);
        return -1;
    }
    fclose(f);
    return value;
}

static char *read_string_from_file(const char *path) {
    static char buf[128];
    FILE *f = fopen(path, "r");
    if (!f) return NULL;

    if (fgets(buf, sizeof(buf), f) == NULL) {
        fclose(f);
        return NULL;
    }
    fclose(f);

    // Remove trailing newline
    size_t len = strlen(buf);
    if (len > 0 && buf[len - 1] == '\n') {
        buf[len - 1] = '\0';
    }

    return buf;
}

struct battery *battery_create(struct bar *bar) {
    struct battery *bat = calloc(1, sizeof(struct battery));
    if (!bat) return NULL;

    bat->bar = bar;
    bat->available = false;
    bat->percentage = 100;
    bat->state = BATTERY_UNKNOWN;

    char *path = find_battery_path();
    if (path) {
        strncpy(bat->path, path, MAX_BATTERY_PATH - 1);
        bat->available = true;
        battery_update(bat);
    }

    return bat;
}

void battery_destroy(struct battery *bat) {
    free(bat);
}

void battery_update(struct battery *bat) {
    if (!bat->available) return;

    char buf[256];

    // Read capacity
    snprintf(buf, sizeof(buf), "%s/capacity", bat->path);
    int capacity = read_int_from_file(buf);
    if (capacity >= 0) {
        bat->percentage = capacity;
    }

    // Read status
    snprintf(buf, sizeof(buf), "%s/status", bat->path);
    char *status = read_string_from_file(buf);
    if (status) {
        if (strcmp(status, "Charging") == 0) {
            bat->state = BATTERY_CHARGING;
        } else if (strcmp(status, "Full") == 0) {
            bat->state = BATTERY_FULL;
        } else if (strcmp(status, "Discharging") == 0) {
            bat->state = BATTERY_DISCHARGING;
        } else {
            bat->state = BATTERY_UNKNOWN;
        }
    }

    // Compute time remaining (minutes)
    bat->time_remaining = -1;

    int energy_now = -1, power_now = -1;
    int charge_now = -1, current_now = -1;

    snprintf(buf, sizeof(buf), "%s/energy_now", bat->path);
    energy_now = read_int_from_file(buf);
    snprintf(buf, sizeof(buf), "%s/power_now", bat->path);
    power_now = read_int_from_file(buf);

    if (energy_now < 0 || power_now < 0) {
        snprintf(buf, sizeof(buf), "%s/charge_now", bat->path);
        charge_now = read_int_from_file(buf);
        snprintf(buf, sizeof(buf), "%s/current_now", bat->path);
        current_now = read_int_from_file(buf);
    }

    int numerator = -1, denominator = -1;
    if (energy_now >= 0 && power_now > 0) {
        numerator = energy_now;
        denominator = power_now;
    } else if (charge_now >= 0 && current_now > 0) {
        numerator = charge_now;
        denominator = current_now;
    }

    if (numerator >= 0 && denominator > 0) {
        if (bat->state == BATTERY_DISCHARGING) {
            bat->time_remaining = (int)((60.0 * numerator) / denominator);
        } else if (bat->state == BATTERY_CHARGING) {
            int full = -1;
            snprintf(buf, sizeof(buf), "%s/energy_full", bat->path);
            full = read_int_from_file(buf);
            if (full < 0) {
                snprintf(buf, sizeof(buf), "%s/charge_full", bat->path);
                full = read_int_from_file(buf);
            }
            if (full >= 0 && full > numerator) {
                bat->time_remaining = (int)((60.0 * (full - numerator)) / denominator);
            }
        }
    }
}

void battery_render(struct battery *bat) {
    (void)bat;
    // Rendering is handled in gl-renderer.c
}