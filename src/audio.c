#include "common.h"
#include "audio.h"

#define RING (RATE * 5)              /* 5 s of mono */
#define FADE_SAMPLES 2205            /* 50 ms */

static int16_t ring[RING];
static volatile uint32_t head, tail;  /* head: producer (update), tail: consumer (callback) */
static volatile int playing, muted, starving, expect_gap;
static volatile int underruns;
static volatile int hiss_target_q15, hiss_q15;
static volatile int gain_q15;         /* fade in/out */
static volatile uint32_t played;
static volatile int flush_req;
static volatile uint32_t flush_to;
static uint32_t rng = 0x12345678;
static volatile int silent;          /* test runs: run everything, output zeros */
static int16_t *tap;                 /* test runs: a copy of the output (video takes) */
static volatile int tap_len, tap_cap;

static int callback(void *ctx, int16_t *left, int16_t *right, int len) {
    (void)ctx; (void)right;
    uint32_t t = tail, h = head;
    int g = gain_q15;
    if (flush_req) { t = flush_to; flush_req = 0; starving = 0; g = 0; }
    int target =(playing && !muted) ? 32767 : 0;
    int hz = hiss_q15, htarget = muted ? 0 : hiss_target_q15;
    int step = 32767 / FADE_SAMPLES + 1;
    for (int i = 0; i < len; i++) {
        int s = 0;
        if (playing) {
            if (t != h) {
                s = ring[t];
                t = t + 1 == RING ? 0 : t + 1;
                played++;
                starving = 0;
            } else if (!starving) {
                starving = 1;
                if (!expect_gap) underruns++;
            }
        }
        if (g < target) { g += step; if (g > target) g = target; }
        else if (g > target) { g -= step; if (g < target) g = target; }
        if (hz < htarget) hz += 4; else if (hz > htarget) hz -= 4;
        int v = (s * g) >> 15;
        if (hz > 0) {
            rng = rng * 1664525u + 1013904223u;
            int n = (int)(rng >> 16) - 32768;          /* white */
            v += (n * hz) >> 15;
        }
        left[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
    }
    if (tap) {
        int k = tap_cap - tap_len < len ? tap_cap - tap_len : len;
        if (k > 0) { memcpy(tap + tap_len, left, (size_t)k * sizeof left[0]); tap_len += k; }
    }
    if (silent) memset(left, 0, (size_t)len * sizeof left[0]);
    tail = t;
    gain_q15 = g;
    hiss_q15 = hz;
    return 1;
}

void audio_init(void) {
    pd->sound->addSource(callback, NULL, 0);
}

int audio_buffered(void) {
    uint32_t h = head, t = flush_req ? flush_to : tail;
    return (int)(h >= t ? h - t : RING - t + h);
}
int audio_space(void) { return RING - 1 - audio_buffered(); }

void audio_push(const int16_t *s, int n) {
    int space = audio_space();
    if (n > space) n = space;
    uint32_t h = head;
    for (int i = 0; i < n; i++) {
        ring[h] = s[i];
        h = h + 1 == RING ? 0 : h + 1;
    }
    head = h;
    if (n > 0) expect_gap = 0;
}

/* The callback owns tail: ask it to skip to where head is now. */
void audio_flush(void) {
    flush_to = head;
    flush_req = 1;
}

void audio_play(int on) {
    if (on && !playing) starving = 0;
    playing = on;
}
int audio_playing(void) { return playing; }

void audio_hiss(float level) {
    if (level < 0) level = 0;
    if (level > 1) level = 1;
    hiss_target_q15 = (int)(level * 2600);       /* about -22 dBFS at full */
}

void audio_mute(int m) { muted = m; }
int audio_muted(void) { return muted; }
int audio_underruns(void) { return underruns; }
void audio_expect_gap(void) { expect_gap = 1; }
uint32_t audio_played_samples(void) { return played; }
void audio_test_silent(int on) { silent = on; }
int audio_tap_begin(int seconds) {
    tap_len = 0;
    tap_cap = RATE * seconds;
    tap = malloc((size_t)tap_cap * sizeof *tap);
    return tap ? 0 : -1;
}
int16_t *audio_tap_end(int *len) {
    int16_t *t = tap;
    *len = tap_len;
    tap = NULL;
    return t;
}
