#include "battery_history.h"

#include <ctype.h>
#include <glob.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#define UPOWER_HISTORY_GLOB "/var/lib/upower/history-charge-*.dat"

static int64_t now_secs(void) { return (int64_t)time(NULL); }

static void ensure_capacity(struct battery_history *h, size_t need) {
    if (h->capacity >= need) return;
    size_t cap = h->capacity ? h->capacity : 512;
    while (cap < need) cap *= 2;
    h->records = realloc(h->records, cap * sizeof(struct history_record));
    h->capacity = cap;
}

static void ensure_down_capacity(struct battery_history *h, size_t need) {
    if (h->down_capacity >= need) return;
    size_t cap = h->down_capacity ? h->down_capacity : 32;
    while (cap < need) cap *= 2;
    h->downs = realloc(h->downs, cap * sizeof(struct down_interval));
    h->down_capacity = cap;
}

static void add_down(struct battery_history *h, int64_t start, int64_t end) {
    if (end <= start) return;
    ensure_down_capacity(h, h->down_count + 1);
    h->downs[h->down_count++] = (struct down_interval){ start, end };
}

static int cmp_down(const void *a, const void *b) {
    const struct down_interval *da = a, *db = b;
    if (da->start < db->start) return -1;
    if (da->start > db->start) return  1;
    return 0;
}

/* Parse boot intervals from `journalctl --list-boots -o json`. Off intervals
 * are the gaps between boot N's last_entry and boot N+1's first_entry. */
static void parse_boots(struct battery_history *h, int64_t cutoff) {
    FILE *p = popen("journalctl --list-boots -o json --no-pager 2>/dev/null", "r");
    if (!p) return;

    /* Output is one big JSON array on one line, but entries are well-formed
     * objects. Read the whole thing then walk it. */
    char *buf = NULL;
    size_t cap = 0, len = 0;
    char chunk[4096];
    size_t n;
    while ((n = fread(chunk, 1, sizeof(chunk), p)) > 0) {
        if (len + n + 1 > cap) {
            cap = (cap ? cap * 2 : 8192);
            while (cap < len + n + 1) cap *= 2;
            buf = realloc(buf, cap);
        }
        memcpy(buf + len, chunk, n);
        len += n;
    }
    pclose(p);
    if (!buf || len == 0) { free(buf); return; }
    buf[len] = '\0';

    int64_t prev_last = -1;
    const char *cursor = buf;
    while ((cursor = strstr(cursor, "\"first_entry\":")) != NULL) {
        long long first_us = 0, last_us = 0;
        if (sscanf(cursor, "\"first_entry\":%lld", &first_us) != 1) break;
        const char *last_p = strstr(cursor, "\"last_entry\":");
        if (!last_p) break;
        if (sscanf(last_p, "\"last_entry\":%lld", &last_us) != 1) break;

        int64_t first_s = first_us / 1000000;
        int64_t last_s  = last_us  / 1000000;

        if (prev_last >= 0 && first_s > prev_last) {
            int64_t s = prev_last, e = first_s;
            if (e > cutoff) add_down(h, s < cutoff ? cutoff : s, e);
        }
        prev_last = last_s;
        cursor = last_p + 1;
    }
    free(buf);
}

/* Parse `journalctl -t systemd-sleep -o short-unix` for suspend/resume pairs.
 * Each "Performing sleep" line is closed by the next "System returned" line. */
static void parse_sleeps(struct battery_history *h, int64_t cutoff) {
    char cmd[256];
    snprintf(cmd, sizeof(cmd),
        "journalctl -t systemd-sleep --since=@%lld -o short-unix --no-pager 2>/dev/null",
        (long long)cutoff);
    FILE *p = popen(cmd, "r");
    if (!p) return;

    char line[512];
    int64_t pending_start = -1;
    while (fgets(line, sizeof(line), p)) {
        double ts = 0;
        if (sscanf(line, "%lf", &ts) != 1) continue;
        int64_t t = (int64_t)ts;

        if (strstr(line, "Performing sleep operation")) {
            pending_start = t;
        } else if (strstr(line, "System returned from sleep operation")) {
            if (pending_start > 0) {
                add_down(h, pending_start, t);
                pending_start = -1;
            }
        }
    }
    pclose(p);

    /* Unclosed suspend (current sleep, or boot interrupted resume): close at now. */
    if (pending_start > 0) add_down(h, pending_start, now_secs());
}

static void merge_downs(struct battery_history *h) {
    if (h->down_count < 2) return;
    qsort(h->downs, h->down_count, sizeof(*h->downs), cmp_down);
    size_t w = 0;
    for (size_t r = 0; r < h->down_count; r++) {
        if (w == 0 || h->downs[r].start > h->downs[w - 1].end) {
            h->downs[w++] = h->downs[r];
        } else if (h->downs[r].end > h->downs[w - 1].end) {
            h->downs[w - 1].end = h->downs[r].end;
        }
    }
    h->down_count = w;
}

static uint8_t parse_state(const char *s) {
    if (!s) return HIST_UNKNOWN;
    if (strcmp(s, "discharging") == 0)   return HIST_DISCHARGING;
    if (strcmp(s, "charging") == 0)      return HIST_CHARGING;
    if (strcmp(s, "fully-charged") == 0) return HIST_FULL;
    if (strcmp(s, "pending-charge") == 0) return HIST_FULL;
    return HIST_UNKNOWN;
}

static int cmp_record(const void *a, const void *b) {
    const struct history_record *ra = a, *rb = b;
    if (ra->timestamp < rb->timestamp) return -1;
    if (ra->timestamp > rb->timestamp) return  1;
    return 0;
}

static void parse_file(struct battery_history *h, const char *path,
                       int64_t cutoff) {
    FILE *f = fopen(path, "r");
    if (!f) return;

    char line[256];
    while (fgets(line, sizeof(line), f)) {
        int64_t ts;
        double pct;
        char state[64];
        if (sscanf(line, "%lld %lf %63s", (long long *)&ts, &pct, state) != 3)
            continue;
        if (ts < cutoff) continue;

        int p = (int)(pct + 0.5);
        if (p < 0) p = 0; else if (p > 100) p = 100;

        ensure_capacity(h, h->count + 1);
        h->records[h->count++] = (struct history_record){
            .timestamp = ts,
            .percentage = (uint8_t)p,
            .state = parse_state(state),
            ._pad = 0,
        };
    }
    fclose(f);
}

bool battery_history_refresh(struct battery_history *h) {
    if (!h) return false;
    h->count = 0;
    h->down_count = 0;

    /* Read more than the display window so autonomy-since-last-charge can
     * reach back further than 24h if needed. */
    int64_t cutoff = now_secs() - 7 * 24 * 3600;

    glob_t g;
    if (glob(UPOWER_HISTORY_GLOB, 0, NULL, &g) == 0) {
        for (size_t i = 0; i < g.gl_pathc; i++) {
            parse_file(h, g.gl_pathv[i], cutoff);
        }
        globfree(&g);
    }

    if (h->count > 1) {
        qsort(h->records, h->count, sizeof(*h->records), cmp_record);
        /* Drop duplicate timestamps from overlapping files. */
        size_t w = 1;
        for (size_t r = 1; r < h->count; r++) {
            if (h->records[r].timestamp == h->records[w - 1].timestamp) continue;
            h->records[w++] = h->records[r];
        }
        h->count = w;
    }

    parse_boots(h, cutoff);
    parse_sleeps(h, cutoff);
    merge_downs(h);

    h->last_refresh_ts = now_secs();
    return true;
}

struct battery_history *battery_history_create(void) {
    struct battery_history *h = calloc(1, sizeof(*h));
    if (!h) return NULL;
    battery_history_refresh(h);
    return h;
}

void battery_history_destroy(struct battery_history *h) {
    if (!h) return;
    free(h->records);
    free(h->downs);
    free(h);
}

/* Sum of [start, end) overlap with the merged down intervals. */
static int64_t down_time_in_range(struct battery_history *h,
                                  int64_t start, int64_t end) {
    int64_t down = 0;
    for (size_t i = 0; i < h->down_count; i++) {
        int64_t a = h->downs[i].start;
        int64_t b = h->downs[i].end;
        if (b <= start) continue;
        if (a >= end) break;
        int64_t lo = a < start ? start : a;
        int64_t hi = b > end ? end : b;
        if (hi > lo) down += hi - lo;
    }
    return down;
}

static int64_t awake_time_in_range(struct battery_history *h,
                                   int64_t start, int64_t end) {
    if (end <= start) return 0;
    int64_t span = end - start;
    int64_t down = down_time_in_range(h, start, end);
    return span - down;
}

/* Find the most recent record at or before t. Returns -1 if none. */
static int last_record_before(struct battery_history *h, int64_t t) {
    int last = -1;
    for (size_t k = 0; k < h->count; k++) {
        if (h->records[k].timestamp <= t) last = (int)k;
        else break;
    }
    return last;
}

void battery_history_buckets(struct battery_history *h,
                             struct hour_bucket buckets[HISTORY_HOURS]) {
    int64_t now = now_secs();
    int64_t hour_anchor = now - (now % 3600);

    for (int i = 0; i < HISTORY_HOURS; i++) {
        int64_t bucket_end   = hour_anchor - (HISTORY_HOURS - 1 - i) * 3600 + 3600;
        int64_t bucket_start = bucket_end - 3600;
        if (bucket_end > now) bucket_end = now;

        struct hour_bucket *b = &buckets[i];
        b->has_data = false;
        b->percentage = 0;
        b->color = HOUR_EMPTY;

        if (!h) continue;

        int64_t span = bucket_end - bucket_start;
        if (span <= 0) continue;

        int64_t awake = awake_time_in_range(h, bucket_start, bucket_end);

        /* Mostly down (suspended or off per journalctl) → purple, even if
         * we have no battery samples for it. */
        if (awake * 2 < span) {
            b->has_data = true;
            int idx = last_record_before(h, bucket_end);
            if (idx >= 0) b->percentage = h->records[idx].percentage;
            b->color = HOUR_SLEEP;
            continue;
        }

        /* Awake hour: need a percentage to color it. */
        int idx = last_record_before(h, bucket_end);
        if (idx < 0) continue;

        b->has_data = true;
        b->percentage = h->records[idx].percentage;

        if (b->percentage < 10)      b->color = HOUR_RED;
        else if (b->percentage < 30) b->color = HOUR_YELLOW;
        else                          b->color = HOUR_GREEN;
    }
}

int64_t battery_history_autonomy_secs(struct battery_history *h) {
    if (!h || h->count == 0) return -1;

    int64_t charge_end = -1;
    for (size_t i = h->count; i-- > 0; ) {
        uint8_t s = h->records[i].state;
        if (s == HIST_CHARGING || s == HIST_FULL) {
            charge_end = h->records[i].timestamp;
            break;
        }
    }
    if (charge_end < 0) charge_end = h->records[0].timestamp;

    return awake_time_in_range(h, charge_end, now_secs());
}
