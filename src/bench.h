#pragma once
#include <stdint.h>
#include "sched.h"

/* The two-minute test: decode speed for each audio format Static plays (from
 * clips bundled in the app, so every Playdate decodes the same bytes), then
 * real streaming from today's schedule. Nothing is played aloud. */

#define BENCH_CLIPS 3
#define BENCH_STREAMS 4

typedef struct {
    const char *name;        /* "MP3 128k", ... */
    float decode_s, audio_s; /* CPU seconds spent decoding, seconds of audio produced */
    int frames, errors, ok;
} bench_clip_t;

typedef struct {
    char station[24];
    int fmt_mp4;
    int status, hops;
    uint32_t ttfb_ms, ms;    /* first byte incl. every redirect; whole read */
    int bytes;
    float kbps;              /* KB/s over the body */
    char err[40];
} bench_stream_t;

typedef struct {
    bench_clip_t clip[BENCH_CLIPS];
    bench_stream_t stream[BENCH_STREAMS];
    int nstreams;
    uint32_t sched_ms; int sched_kb; int sched_ok;
    int clock_known; int64_t clock_off_ms;   /* device minus server */
    int step;                /* 0..BENCH_STEPS, current */
    int done;
    float frame_cpu_ms;      /* average update cost while idle-ish (UI only) */
} bench_t;

#define BENCH_STEPS (BENCH_CLIPS + 2)    /* clips, schedule, streams */

void bench_start(bench_t *b);
/* Work until cpu_s() reaches budget_s. The schedule, once fetched, is handed
 * out through *sched_out (caller owns it). */
void bench_update(bench_t *b, float budget_s, sched_t **sched_out);
const char *bench_step_name(int step);
