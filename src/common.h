#pragma once
#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include "pd_api.h"

extern PlaydateAPI *pd;

#ifndef APP_VERSION
#define APP_VERSION "0.1.0"
#endif
#define USER_AGENT "StaticRadio/playdate-preview (+https://staticradio.app)"
#define SCHEDULE_URL "https://data.staticradio.app/schedule-wear.json"
#define EVENT_URL "https://staticradio.app/api/event"

/* Phone parity (sync_controller.dart _kInterSegmentGapMillis). */
#define GAP_MS 1500
/* Phone parity (radio_audio_engine.dart): bulletins never play past 30 s. */
#define BULLETIN_CAP_MS 30000

#define RATE 44100

extern float app_fps;   /* update rate over the last 10 s */

void logf_(const char *fmt, ...);
#define LOG(...) logf_(__VA_ARGS__)

/* Monotonic milliseconds (pauses while the device sleeps). */
static inline uint32_t tick_ms(void) { return pd->system->getCurrentTimeMilliseconds(); }

/* Unix wall clock in ms, corrected by the server-time offset once known. */
int64_t wall_ms(void);
/* Device clock minus the true time, ms (0 until measured); 1 once measured. */
int clock_offset(int64_t *off);
void clock_set_true_unix_s(int64_t unix_s, const char *source);
/* Fallback clock source: an HTTP Date header ("Sun, 01 Nov 2026 17:15:06 GMT"). */
void clock_note_http_date(const char *date);

/* Unix seconds from a UTC calendar date (no timegm in newlib). */
int64_t unix_from_utc(int y, int mo, int d, int h, int mi, int s);

/* Seconds of CPU time since the start of this update (high-resolution timer,
 * reset at the top of every update). */
static inline float cpu_s(void) { return pd->system->getElapsedTime(); }

/* Short-lived file writes in Data/ (breadcrumbs, settings). */
int data_write(const char *name, const void *buf, int len);
/* Returns a malloc'd NUL-terminated copy, or NULL. */
char *data_read(const char *name, int *len);
