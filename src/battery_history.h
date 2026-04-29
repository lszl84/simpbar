#ifndef BATTERY_HISTORY_H
#define BATTERY_HISTORY_H

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#define HISTORY_HOURS 24
#define HISTORY_REFRESH_INTERVAL_SEC 600   /* re-read upower + journal every 10 min */

enum history_state {
    HIST_CHARGING    = 0,
    HIST_DISCHARGING = 1,
    HIST_FULL        = 2,
    HIST_UNKNOWN     = 3,
};

struct history_record {
    int64_t timestamp;
    uint8_t percentage;
    uint8_t state;
    uint16_t _pad;
};

enum hour_color {
    HOUR_GREEN  = 0,
    HOUR_YELLOW = 1,
    HOUR_RED    = 2,
    HOUR_SLEEP  = 3,
    HOUR_EMPTY  = 4,
};

struct hour_bucket {
    bool has_data;
    int percentage;
    enum hour_color color;
};

struct down_interval {
    int64_t start;
    int64_t end;
};

struct battery_history {
    struct history_record *records;
    size_t count;
    size_t capacity;

    /* System-level down intervals (suspend + off) parsed from journalctl.
     * Authoritative source for sleep/off detection — upower gaps are noisy. */
    struct down_interval *downs;
    size_t down_count;
    size_t down_capacity;

    int64_t last_refresh_ts;
};

struct battery_history *battery_history_create(void);
void battery_history_destroy(struct battery_history *h);

/* Re-read upower's on-disk history. Cheap (parses text files); call from a
 * 10-min timer. Returns true if data changed. */
bool battery_history_refresh(struct battery_history *h);

void battery_history_buckets(struct battery_history *h,
                             struct hour_bucket buckets[HISTORY_HOURS]);

/* Awake (non-sleep) time since the most recent charging/full sample. */
int64_t battery_history_autonomy_secs(struct battery_history *h);

#endif
