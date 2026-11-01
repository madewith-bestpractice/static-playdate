#include "common.h"
#include "dec.h"
#include "third_party/helix/libhelix-mp3/mp3dec.h"
#include "third_party/helix/libhelix-aac/aacdec.h"
#include <math.h>

#define IN_CAP 32768
#define PCM_MAX (2048 * 2)      /* AAC+SBR: 2048 samples per channel, stereo */
#define MIN_FRAME_BYTES 2048    /* decode only with a whole frame buffered (until EOF) */

struct dec {
    int fmt;
    HMP3Decoder mp3;
    HAACDecoder aac;
    uint8_t in[IN_CAP];
    int len, off;
    int64_t pos;                /* file offset of in[0] */
    int eof;
    const mp4idx_t *idx;
    mp4iter_t it;
    uint64_t s_off; uint32_t s_size; int have_sample;
    int wav_rate, wav_ch;
    int16_t pcm[PCM_MAX];
    int in_rate, ch;
    uint32_t phase, step;       /* Q16 resampler position between last and next input */
    int last;
    int64_t out_samples, start_ms;
    int gain_q12;
    int frames, errors, sbr;
};

dec_t *dec_new(int fmt, int64_t first_byte, int64_t start_ms,
               const mp4idx_t *idx, uint32_t first_sample, int wav_rate, int wav_ch) {
    dec_t *d = calloc(1, sizeof *d);
    if (!d) return NULL;
    d->fmt = fmt;
    d->pos = first_byte;
    d->start_ms = start_ms;
    d->gain_q12 = 4096;
    if (fmt == FMT_MP3) d->mp3 = MP3InitDecoder();
    else if (fmt == FMT_MP4 || fmt == FMT_ADTS) {
        d->aac = AACInitDecoder();
        if (d->aac && fmt == FMT_MP4) {
            AACFrameInfo fi = {0};
            fi.nChans = idx->channels;
            fi.sampRateCore = idx->core_rate;
            fi.profile = AAC_PROFILE_LC;
            AACSetRawBlockParams(d->aac, 0, &fi);
            d->idx = idx;
            if (mp4_iter_seek(idx, first_sample, &d->it)) { dec_free(d); return NULL; }
        }
    } else if (fmt == FMT_WAV) { d->wav_rate = wav_rate; d->wav_ch = wav_ch; }
    if ((fmt == FMT_MP3 && !d->mp3) || ((fmt == FMT_MP4 || fmt == FMT_ADTS) && !d->aac) || fmt == FMT_UNKNOWN) { dec_free(d); return NULL; }
    return d;
}

void dec_free(dec_t *d) {
    if (!d) return;
    if (d->mp3) MP3FreeDecoder(d->mp3);
    if (d->aac) AACFreeDecoder(d->aac);
    free(d);
}

int dec_space(const dec_t *d) { return IN_CAP - (d->len - d->off); }

void dec_push(dec_t *d, const uint8_t *b, int n) {
    if (d->len + n > IN_CAP && d->off) {
        memmove(d->in, d->in + d->off, (size_t)(d->len - d->off));
        d->len -= d->off; d->pos += d->off; d->off = 0;
    }
    if (n > IN_CAP - d->len) n = IN_CAP - d->len;
    memcpy(d->in + d->len, b, (size_t)n);
    d->len += n;
}

void dec_end_input(dec_t *d) { d->eof = 1; }

static int emit(dec_t *d, const int16_t *pcm, int frames, int ch, int rate, int16_t *out, int cap) {
    if (rate <= 0 || frames <= 0) return 0;
    if (rate != d->in_rate) { d->in_rate = rate; d->step = (uint32_t)(((uint64_t)rate << 16) / RATE); }
    d->ch = ch;
    int n = 0, last = d->last;
    uint32_t ph = d->phase, step = d->step;
    for (int i = 0; i < frames; i++) {
        int s = ch == 2 ? (pcm[2 * i] + pcm[2 * i + 1]) >> 1 : pcm[i];
        while (ph < 65536 && n < cap) {
            out[n++] = (int16_t)(last + (((s - last) * (int)(ph >> 1)) >> 15));
            ph += step;
        }
        ph -= 65536;
        last = s;
    }
    d->phase = ph; d->last = last;
    if (d->gain_q12 != 4096) {
        int g = d->gain_q12;
        for (int i = 0; i < n; i++) {
            int v = (out[i] * g) >> 12;
            out[i] = (int16_t)(v > 32767 ? 32767 : v < -32768 ? -32768 : v);
        }
    }
    d->out_samples += n;
    return n;
}

static int step_mp3(dec_t *d, int16_t *out, int cap) {
    int avail = d->len - d->off;
    if (avail < MIN_FRAME_BYTES && !d->eof) return -1;
    if (avail <= 4) return d->eof ? -2 : -1;
    int s = MP3FindSyncWord(d->in + d->off, avail);
    if (s < 0) { d->off = d->len - 3; return d->eof ? -2 : -1; }
    d->off += s;
    unsigned char *p = d->in + d->off;
    int left = d->len - d->off;
    int err = MP3Decode(d->mp3, &p, &left, d->pcm, 0);
    if (err == ERR_MP3_INDATA_UNDERFLOW) { if (d->eof) return -2; return -1; }
    d->off = (int)(p - d->in);
    if (err == ERR_MP3_MAINDATA_UNDERFLOW) return 0;     /* bit reservoir warming up */
    if (err) { d->errors++; if (d->off < d->len) d->off++; return 0; }
    MP3FrameInfo fi;
    MP3GetLastFrameInfo(d->mp3, &fi);
    if (fi.outputSamps <= 0 || fi.nChans <= 0) return 0;
    d->frames++;
    return emit(d, d->pcm, fi.outputSamps / fi.nChans, fi.nChans, fi.samprate, out, cap);
}

static int after_aac(dec_t *d, int16_t *out, int cap) {
    AACFrameInfo fi;
    AACGetLastFrameInfo(d->aac, &fi);
    if (fi.outputSamps <= 0 || fi.nChans <= 0) return 0;
    d->frames++;
    d->sbr = fi.sampRateOut >= 2 * fi.sampRateCore;
    return emit(d, d->pcm, fi.outputSamps / fi.nChans, fi.nChans, fi.sampRateOut, out, cap);
}

static int step_adts(dec_t *d, int16_t *out, int cap) {
    int avail = d->len - d->off;
    if (avail < MIN_FRAME_BYTES && !d->eof) return -1;
    if (avail <= 7) return d->eof ? -2 : -1;
    int s = AACFindSyncWord(d->in + d->off, avail);
    if (s < 0) { d->off = d->len - 1; return d->eof ? -2 : -1; }
    d->off += s;
    unsigned char *p = d->in + d->off;
    int left = d->len - d->off;
    int err = AACDecode(d->aac, &p, &left, d->pcm);
    if (err == ERR_AAC_INDATA_UNDERFLOW) return d->eof ? -2 : -1;
    d->off = (int)(p - d->in);
    if (err) { d->errors++; if (d->off < d->len) d->off++; return 0; }
    return after_aac(d, out, cap);
}

static int step_mp4(dec_t *d, int16_t *out, int cap) {
    if (!d->have_sample) {
        if (!mp4_iter_next(d->idx, &d->it, &d->s_off, &d->s_size)) return -2;
        d->have_sample = 1;
    }
    if (d->s_size == 0 || d->s_size > IN_CAP / 2) { d->have_sample = 0; d->errors++; return 0; }
    int64_t lo = d->pos, hi = d->pos + d->len;
    if ((int64_t)d->s_off < lo) { d->have_sample = 0; d->errors++; return 0; }   /* already gone */
    if ((int64_t)d->s_off >= hi) { d->off = d->len; return d->eof ? -2 : -1; }   /* gap: drop what we have */
    d->off = (int)((int64_t)d->s_off - d->pos);
    if ((int64_t)d->s_off + d->s_size > hi) return d->eof ? -2 : -1;
    unsigned char *p = d->in + d->off;
    int left = (int)d->s_size;
    int err = AACDecode(d->aac, &p, &left, d->pcm);
    d->off += (int)d->s_size;
    d->have_sample = 0;
    if (err) { d->errors++; return 0; }
    return after_aac(d, out, cap);
}

static int step_wav(dec_t *d, int16_t *out, int cap) {
    int fb = 2 * d->wav_ch;
    int avail = d->len - d->off;
    int frames = avail / fb;
    if (!frames) return d->eof ? -2 : -1;
    int maxf = PCM_MAX / d->wav_ch;
    if (frames > maxf) frames = maxf;
    if (frames > 1024) frames = 1024;
    const uint8_t *b = d->in + d->off;
    for (int i = 0; i < frames * d->wav_ch; i++) d->pcm[i] = (int16_t)(b[2 * i] | b[2 * i + 1] << 8);
    d->off += frames * fb;
    d->frames++;
    return emit(d, d->pcm, frames, d->wav_ch, d->wav_rate, out, cap);
}

int dec_step(dec_t *d, int16_t *out, int cap) {
    switch (d->fmt) {
    case FMT_MP3: return step_mp3(d, out, cap);
    case FMT_ADTS: return step_adts(d, out, cap);
    case FMT_MP4: return step_mp4(d, out, cap);
    case FMT_WAV: return step_wav(d, out, cap);
    }
    return -2;
}

int64_t dec_pos_ms(const dec_t *d) { return d->start_ms + d->out_samples * 1000 / RATE; }
int dec_in_rate(const dec_t *d) { return d->in_rate; }
int dec_channels(const dec_t *d) { return d->ch; }
int dec_errors(const dec_t *d) { return d->errors; }
int dec_frames(const dec_t *d) { return d->frames; }
int dec_is_sbr(const dec_t *d) { return d->sbr; }
void dec_set_gain_db(dec_t *d, float db) {
    if (db > 6) db = 6;         /* the phone's loudness boost ceiling */
    if (db < -18) db = -18;
    d->gain_q12 = (int)(4096.0f * powf(10.0f, db / 20.0f));
}
