#pragma once
#include <stdint.h>
#include "mp4.h"

/* Compressed bytes in, 44.1 kHz mono s16 out (Helix MP3 / AAC, or WAV).
 * The Playdate mixes mono; a stereo callback crashed in other people's tests,
 * and the speaker is mono anyway. */
enum { FMT_UNKNOWN, FMT_MP3, FMT_MP4, FMT_ADTS, FMT_WAV };

typedef struct dec dec_t;

/* first_byte: file offset of the first byte that will be pushed.
 * start_ms: media time at that byte (exact for MP4, estimated for MP3).
 * For FMT_MP4, idx (borrowed, must outlive the decoder) and first_sample. */
dec_t *dec_new(int fmt, int64_t first_byte, int64_t start_ms,
               const mp4idx_t *idx, uint32_t first_sample, int wav_rate, int wav_ch);
void dec_free(dec_t *d);

int dec_space(const dec_t *d);                    /* bytes push will take now */
void dec_push(dec_t *d, const uint8_t *b, int n);
void dec_end_input(dec_t *d);                     /* no more bytes coming */

/* Decode one frame. > 0 samples written to out (cap >= 4096), 0 a frame that
 * produced nothing (bit-reservoir warmup, bad frame), -1 needs more input,
 * -2 finished (or unrecoverable). */
int dec_step(dec_t *d, int16_t *out, int cap);

int64_t dec_pos_ms(const dec_t *d);               /* media time of the next sample out */
int dec_in_rate(const dec_t *d);
int dec_channels(const dec_t *d);
int dec_errors(const dec_t *d);
int dec_frames(const dec_t *d);
int dec_is_sbr(const dec_t *d);                   /* AAC with SBR (HE-AAC) */
void dec_set_gain_db(dec_t *d, float db);
