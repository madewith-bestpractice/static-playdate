#pragma once
#include <stdint.h>

/* One mono 44.1 kHz output: a ring the update loop fills and the audio
 * callback drains, plus a soft tuning hiss between stations. */
void audio_init(void);
int audio_buffered(void);              /* samples waiting */
int audio_space(void);                 /* samples that fit */
void audio_push(const int16_t *s, int n);
void audio_flush(void);                /* drop everything queued */
/* Playing: drain the ring (underruns counted). Not playing: silence. */
void audio_play(int on);
int audio_playing(void);
/* 0..1 hiss level (fades). */
void audio_hiss(float level);
void audio_mute(int m);
int audio_muted(void);
/* The queue ran dry while playing. */
int audio_underruns(void);
void audio_expect_gap(void);           /* end of an episode: a dry ring isn't an underrun */
uint32_t audio_played_samples(void);
/* Simulator test runs: the whole pipeline runs, the output is zeros. */
void audio_test_silent(int on);
