#pragma once
#include <stdint.h>
#include "sched.h"

enum { PL_OFF, PL_TUNING, PL_PLAYING, PL_GAP, PL_TROUBLE, PL_UNSUPPORTED };

void player_init(void);
/* The schedule is borrowed; call again (or with NULL) before freeing it. */
void player_set_schedule(const sched_t *s);
void player_tune(int station);              /* -1: off */
/* Network + decode work for this frame, until cpu_s() reaches budget_s. */
void player_update(float budget_s);

int player_state(void);
const char *player_status(void);            /* a short line for the screen, "" when fine */
const ep_t *player_episode(void);           /* live episode on the tuned station, or NULL */
int player_station(void);
int64_t player_drift_ms(void);              /* playback minus live (+ ahead) */
uint32_t player_playing_since(void);        /* tick_ms when sound last started */

/* Lifetime stats for the report. Formats: 0 MP3, 1 AAC-LC, 2 HE-AAC, 3 WAV. */
typedef struct {
    float decode_s[4], audio_s[4];
    int frames[4], errors[4];
    int tunes, tune_ok, tune_fail;
    uint32_t tune_ms_sum, tune_ms_max;      /* tune (click) to first sound */
    int stream_fail, reconnects, resyncs, unsupported;
    uint32_t listen_s;
    int moov_kb_max;                        /* biggest MP4 index fetched */
    uint32_t moov_ms_max;
} player_stats_t;
const player_stats_t *player_stats(void);
