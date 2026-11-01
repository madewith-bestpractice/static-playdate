#pragma once
#include "bench.h"

/* Anonymous test reports to Static's own event endpoint, plus the phone's
 * presence/listener-count contract. One side request at a time. */

void report_init(void);
const char *report_device_id(void);

/* Breadcrumbs: the step we're on, so a crash shows up on the next launch.
 * report_clean_exit() on a normal exit. */
void report_stage(const char *stage);
void report_clean_exit(void);
/* The previous run's last step if it didn't exit cleanly, else NULL. */
const char *report_last_crash(void);

/* Queue reports (sent in the background; 1 queued, 0 if busy). */
int report_send_bench(const bench_t *b);
int report_send_session(void);
int report_send_crash(const char *stage);
/* 0 idle, 1 sending, 2 sent ok, 3 failed (sticky until the next send). */
int report_status(void);

/* Presence on the tuned station ("" = none) and listener counts. */
void report_set_presence(const char *station_id);
int report_count(const char *station_id);     /* -1 unknown */

void report_update(void);
