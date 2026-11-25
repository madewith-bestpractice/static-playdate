/* The tuned station: find the live episode, work out where in the file "now"
 * is, stream from there, and keep playback on the shared live timeline.
 *
 * Probing (what is this file, where does a time offset live) is a small job
 * object (pj_t) so the tuned station and background warming can each run one.
 * Results are cached per episode URL; a warmed episode tunes with a single
 * ranged request.
 *
 * The episode comes down in ranges of about 40 s of audio, read as fast as
 * they arrive into a buffer of about 90 s, with the connection closed between
 * them. The Playdate's network can't have a second connection open while one
 * is downloading (net.c), and a stream held open at playback speed would
 * leave no room for anything else: the next show, the listener count. */
#include <math.h>
#include "common.h"
#include "player.h"
#include "audio.h"
#include "dec.h"
#include "mp3probe.h"
#include "mp4.h"
#include "net.h"

#define HEAD_BYTES 32768   /* the tag plus ~16 KB of frames, to measure the byte rate */
#define MOOV_MAX (4 * 1024 * 1024)
#define PC_N 32
#define TARGET_MS 3000           /* keep this much decoded ahead */
#define PREROLL_MS 500
#define RESYNC_MS 400            /* drift beyond this is corrected */
#define WARM_AHEAD_S 45          /* resolve the next episode this early */
#define WARM_REST_MS 30000       /* after a failed background probe, leave the network alone this long */
#define GIVE_UP_FAILS 6          /* then stop saying "reconnecting" and retry once a minute */
#define GIVE_UP_RETRY_MS 60000
/* Decoding speed, as decode time over audio time, judged after JUDGE_S of a
 * stream's audio. The radio gives the decoder about two thirds of each frame
 * while the dial moves and four fifths while it rests. */
#define JUDGE_S 4
#define LITE_ABOVE 0.6f          /* switch the stream to lite mode */
#define HEAVY_ABOVE 0.8f         /* in lite mode (or none), while buffering: too heavy to play */
#define BUF_S 90                 /* compressed audio kept ahead, at most */
#define CHUNK_S 40               /* fetched per request */
#define SBUF_MAX (2 * 1024 * 1024)
#define SBUF_MIN (256 * 1024)
#define CHUNK_RETRIES 3          /* a range that fails is tried again while the buffer lasts */
#define LOW_S 30                 /* less than this ahead: ask for the next range now, even a partial one */
#define SKIP_MS 1500             /* further than this behind live: skip bytes instead of decoding them */

/* ---------- probe cache ---------- */

typedef struct {
    uint32_t key;
    char *final;
    int fmt;
    mp3info_t mp3;
    int wav_rate, wav_ch;
    int64_t wav_data;
    int64_t moov_off, moov_size, total;
    uint32_t at;
} probe_t;

static probe_t cache[PC_N];

static uint32_t hash(const char *s) {
    uint32_t h = 2166136261u;
    while (*s) { h ^= (uint8_t)*s++; h *= 16777619u; }
    return h ? h : 1;
}

static probe_t *cache_find(const char *url) {
    uint32_t k = hash(url);
    for (int i = 0; i < PC_N; i++)
        if (cache[i].key == k && tick_ms() - cache[i].at < 50u * 60 * 1000) return &cache[i];
    return NULL;
}

static void cache_put(const char *url, const probe_t *p) {
    uint32_t k = hash(url);
    probe_t *slot = NULL;
    for (int i = 0; i < PC_N && !slot; i++) if (cache[i].key == k) slot = &cache[i];
    if (!slot) {
        slot = &cache[0];
        for (int i = 1; i < PC_N; i++) if (cache[i].at < slot->at) slot = &cache[i];
    }
    free(slot->final);
    *slot = *p;
    slot->final = strdup(p->final ? p->final : "");
    slot->key = k;
    slot->at = tick_ms();
}

static void cache_drop(const char *url) {
    uint32_t k = hash(url);
    for (int i = 0; i < PC_N; i++) if (cache[i].key == k) { free(cache[i].final); memset(&cache[i], 0, sizeof cache[i]); }
}

/* one parsed MP4 index (the tuned SHEILA episode's is ~1 MB) */
static struct { uint32_t key; int ok; mp4idx_t idx; } moov_cache;

/* ---------- probe jobs ---------- */

enum { PJ_IDLE, PJ_HEAD, PJ_ID3TAIL, PJ_BOX, PJ_MOOV, PJ_DONE, PJ_FAIL };

typedef struct {
    int st;
    char orig[2048];
    int want_moov;
    hs_t *h;
    uint8_t *buf; int len, cap; int64_t want;
    probe_t p;
    int64_t box_at; int nbox;
    uint32_t t0;
    int prio;
    char err[64];
} pj_t;

static void pj_reset(pj_t *j) {
    if (j->h) hs_close(j->h);
    free(j->buf);
    free(j->p.final);
    memset(j, 0, sizeof *j);
}

static void pj_fail(pj_t *j, const char *why) {
    snprintf(j->err, sizeof j->err, "%s", why);
    if (j->h) { hs_close(j->h); j->h = NULL; }
    j->st = PJ_FAIL;
    LOG("probe failed (%s): %.100s", why, j->orig);
}

static int pj_request(pj_t *j, const char *url, int64_t from, int64_t len, int next_state) {
    if (j->h) hs_close(j->h);
    j->h = hs_open(url, "GET", from, from + len - 1, NULL, 0, NULL);
    if (!j->h) { pj_fail(j, "no memory"); return -1; }
    hs_set_priority(j->h, j->prio);
    if ((int64_t)j->cap < len) {
        uint8_t *nb = realloc(j->buf, (size_t)len);
        if (!nb) { pj_fail(j, "no memory"); return -1; }
        j->buf = nb; j->cap = (int)len;
    }
    j->len = 0; j->want = len;
    j->st = next_state;
    return 0;
}

static void pj_start(pj_t *j, const char *orig, int want_moov, int prio) {
    pj_reset(j);
    j->prio = prio;
    snprintf(j->orig, sizeof j->orig, "%s", orig);
    j->want_moov = want_moov;
    j->t0 = tick_ms();
    pj_request(j, orig, 0, HEAD_BYTES, PJ_HEAD);
}

static uint32_t be32(const uint8_t *p) { return (uint32_t)p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]; }
static uint32_t le32(const uint8_t *p) { return (uint32_t)p[0] | p[1] << 8 | p[2] << 16 | (uint32_t)p[3] << 24; }

static void pj_finish(pj_t *j) {
    if (j->h) { hs_close(j->h); j->h = NULL; }
    j->st = PJ_DONE;
    cache_put(j->orig, &j->p);
    LOG("probe ok in %u ms: fmt %d, %.90s", (unsigned)(tick_ms() - j->t0), j->p.fmt, j->p.final);
}

static void moov_parse(pj_t *j) {
    uint32_t k = hash(j->orig);
    if (moov_cache.ok) { mp4_free(&moov_cache.idx); moov_cache.ok = 0; }
    if (mp4_parse_moov(j->buf, (size_t)j->len, &moov_cache.idx) == 0) { moov_cache.ok = 1; moov_cache.key = k; }
}

/* walk MP4 top-level boxes starting at box_at, using what's in buf if it covers it */
static void mp4_walk(pj_t *j, const uint8_t *b, int64_t b_at, int b_len) {
    while (j->nbox++ < 12) {
        int64_t rel = j->box_at - b_at;
        if (rel < 0 || rel + 16 > b_len) {           /* need the box header from the network */
            if (j->p.total > 0 && j->box_at >= j->p.total) { pj_fail(j, "no moov"); return; }
            pj_request(j, j->p.final, j->box_at, 16, PJ_BOX);
            return;
        }
        const uint8_t *h = b + rel;
        int64_t size = be32(h);
        uint32_t type = be32(h + 4);
        if (size == 1) size = (int64_t)((uint64_t)be32(h + 8) << 32 | be32(h + 12));
        else if (size == 0 && j->p.total > 0) size = j->p.total - j->box_at;
        if (size < 8) { pj_fail(j, "bad box"); return; }
        if (type == 0x6d6f6f76) {                     /* moov */
            j->p.moov_off = j->box_at; j->p.moov_size = size;
            if (size > MOOV_MAX) { pj_fail(j, "index too big"); return; }
            if (!j->want_moov) { pj_finish(j); return; }
            if (rel + size <= b_len && b == j->buf) {  /* already have all of it */
                memmove(j->buf, j->buf + rel, (size_t)size);
                j->len = (int)size;
                moov_parse(j);
                if (!moov_cache.ok) { pj_fail(j, "not AAC"); return; }
                pj_finish(j);
                return;
            }
            pj_request(j, j->p.final, j->box_at, size, PJ_MOOV);
            return;
        }
        j->box_at += size;
    }
    pj_fail(j, "no moov");
}

static void pj_got(pj_t *j) {
    if (j->st == PJ_HEAD) {
        int st = hs_status(j->h);
        if (st != 206 && !(st == 200)) { char m[32]; snprintf(m, sizeof m, "HTTP %d", st); pj_fail(j, m); return; }
        free(j->p.final);
        j->p.final = strdup(hs_url(j->h));
        j->p.total = hs_total(j->h);
        if (st == 200 && j->p.total > HEAD_BYTES * 4) { pj_fail(j, "server can't seek"); return; }
        const uint8_t *b = j->buf;
        int len = j->len;
        if (len < 64) { pj_fail(j, "short file"); return; }
        if (!memcmp(b + 4, "ftyp", 4)) {
            j->p.fmt = FMT_MP4;
            j->box_at = 0;
            mp4_walk(j, b, 0, len);
            return;
        }
        if (!memcmp(b, "RIFF", 4) && !memcmp(b + 8, "WAVE", 4)) {
            for (int p = 12; p + 8 <= len; ) {
                uint32_t sz = le32(b + p + 4);
                if (!memcmp(b + p, "fmt ", 4) && p + 24 <= len) {
                    int fmtc = b[p + 8] | b[p + 9] << 8, bits = b[p + 22] | b[p + 23] << 8;
                    j->p.wav_ch = b[p + 10] | b[p + 11] << 8;
                    j->p.wav_rate = (int)le32(b + p + 12);
                    if (fmtc != 1 || bits != 16 || j->p.wav_ch < 1 || j->p.wav_ch > 2) { pj_fail(j, "WAV format"); return; }
                } else if (!memcmp(b + p, "data", 4)) {
                    if (!j->p.wav_rate) break;
                    j->p.wav_data = p + 8;
                    j->p.fmt = FMT_WAV;
                    pj_finish(j);
                    return;
                }
                p += 8 + (int)sz + (int)(sz & 1);
            }
            pj_fail(j, "WAV header");
            return;
        }
        int64_t id3 = mp3_id3_size(b, (size_t)len);
        if (id3 > 8 * 1024 * 1024) { pj_fail(j, "huge tag"); return; }
        j->p.mp3.audio_start = id3;
        j->p.mp3.total = j->p.total > 0 ? j->p.total : 0;
        if (id3 + 16384 <= len) {
            if (mp3_parse_first_frame(b + id3, (size_t)(len - id3), &j->p.mp3)) { j->p.fmt = FMT_MP3; pj_finish(j); }
            else pj_fail(j, "unknown format");
            return;
        }
        pj_request(j, j->p.final, id3, 16384, PJ_ID3TAIL);
        return;
    }
    if (j->st == PJ_ID3TAIL) {
        if (mp3_parse_first_frame(j->buf, (size_t)j->len, &j->p.mp3)) { j->p.fmt = FMT_MP3; pj_finish(j); }
        else pj_fail(j, "unknown format");
        return;
    }
    if (j->st == PJ_BOX) {
        int64_t at = hs_range_start(j->h);
        mp4_walk(j, j->buf, at, j->len);
        return;
    }
    if (j->st == PJ_MOOV) {
        LOG("index: %d KB in %u ms", j->len / 1024, (unsigned)(tick_ms() - j->t0));
        if (j->len < j->p.moov_size) { pj_fail(j, "index cut short"); return; }
        moov_parse(j);
        if (!moov_cache.ok) { pj_fail(j, "not AAC"); return; }
        pj_finish(j);
    }
}

/* returns 1 when the job is finished (PJ_DONE / PJ_FAIL) */
static int pj_poll(pj_t *j) {
    if (j->st == PJ_IDLE) return 0;
    if (j->st == PJ_DONE || j->st == PJ_FAIL) return 1;
    for (int guard = 0; guard < 8 && j->h; guard++) {
        hs_poll(j->h);
        int s = hs_state(j->h);
        if (s == HS_FAIL) {
            char m[64];
            int st = hs_status(j->h);
            if (st >= 400) snprintf(m, sizeof m, "HTTP %d", st);
            else snprintf(m, sizeof m, "%s", hs_error(j->h));
            pj_fail(j, m);
            return 1;
        }
        if (s != HS_BODY && s != HS_DONE) return 0;
        if (hs_status(j->h) >= 400) { char m[32]; snprintf(m, sizeof m, "HTTP %d", hs_status(j->h)); pj_fail(j, m); return 1; }
        int n = hs_read(j->h, j->buf + j->len, (int)(j->want - j->len));
        if (n > 0) { j->len += n; if (j->len < j->want) continue; }
        else if (n == 0) return 0;
        else if (n == -2) { pj_fail(j, hs_error(j->h)); return 1; }
        pj_got(j);   /* full, or the body ended */
        if (j->st == PJ_DONE || j->st == PJ_FAIL) return 1;
    }
    return 0;
}

/* ---------- player ---------- */

static const sched_t *sch;
static int tuned = -1;
static int state = PL_OFF;
static char status[80];
static pj_t job, warm;
static player_stats_t stats;
/* This stream's decode speed. stats adds up the whole run, so judging a station
 * by it would let one slow show mark every later one as too heavy. */
static float stream_audio_s, stream_decode_s;

typedef enum { S_IDLE, S_PROBE, S_CONNECT, S_ALIGN, S_PLAY, S_GAP, S_WAIT } sub_t;
static sub_t sub;
static char ep_url[2048];
static int64_t slot_start_ms;     /* wall time the live episode began */
static int64_t audio_end_ms;      /* media time to stop at (duration / bulletin cap) */
static float ep_gain;
static hs_t *stream;             /* the range being fetched, NULL between ranges */
static dec_t *dec;
/* the stream buffer: a ring of compressed bytes ahead of the decoder */
static uint8_t *sbuf;
static int sbuf_max;
static int s_cap, s_head, s_len, s_chunk;
static int64_t s_next, s_total;   /* next file byte to fetch; file size, -1 unknown */
static int64_t s_req_from, s_req_len;
static float s_rate;              /* bytes per second of audio (estimated) */
static int s_eof, s_fed_eof, s_fails;
static uint32_t s_retry_at;
static char s_url[4096];
static probe_t s_probe;           /* the stream's format, for seeking within it (final not kept) */
static int fmt_class;             /* stats bucket */
static uint32_t tune_t0;          /* click time, for time-to-sound */
static int tune_counted;
static uint32_t retry_at;
static int fails;
static int64_t play_head_ms = -1; /* media time of the oldest sample in the ring */
static uint32_t ttfb_avg = 900;
static int64_t drift;
static uint32_t settled_at;
static int warm_idx;
static uint32_t warm_rest_until;
static uint32_t listen_mark;
static uint32_t playing_since;
static int16_t pcm[8192];
static char unsupported_why[24]; /* shown with the time to the next show; keep it short */
static float slow_decode = 1;    /* Simulator tests: stretch decode time like a slower CPU */
static int dbg_spoil_link;       /* Simulator tests: break the stream's link once, like an expired signature */
static int dbg_ring_max;         /* Simulator tests: cap the stream buffer */

const player_stats_t *player_stats(void) { return &stats; }
int player_state(void) { return state; }
const char *player_status(void) { return status; }
int player_station(void) { return tuned; }
int64_t player_drift_ms(void) { return drift; }
uint32_t player_playing_since(void) { return playing_since; }

static void set_status(const char *s) { snprintf(status, sizeof status, "%s", s); }

void player_debug_slow_decode(float factor) { slow_decode = factor; }
void player_debug_spoil_link(void) { dbg_spoil_link = 1; }
void player_debug_ring_kb(int kb) { dbg_ring_max = kb * 1024; }

/* Simulator tests: wait out slow_decode times this step's usual decode time.
 * The usual time per output sample follows drops quickly and rises slowly, so
 * a moment's hiccup on the Mac isn't multiplied too. */
static float stretch_decode(float t0, float dt, int n) {
    static float per_sample;
    float x = dt / n;
    if (per_sample <= 0) per_sample = x;
    else if (x < per_sample) per_sample += (x - per_sample) * 0.5f;
    else per_sample += (fminf(x, 2 * per_sample) - per_sample) * 0.02f;
    while (cpu_s() < t0 + per_sample * n * slow_decode) {}
    return cpu_s() - t0;
}
int player_lite(void) { return dec && dec_is_lite(dec); }
float player_ahead_s(void) { return dec && s_rate > 0 ? s_len / s_rate : 0; }
int player_hungry(void) { return dec && sub == S_PLAY && !s_eof && s_len < s_rate * LOW_S; }
float player_decode_load(void) { return dec && stream_audio_s > 1 ? stream_decode_s / stream_audio_s : -1; }

void player_init(void) { memset(&stats, 0, sizeof stats); }

/* live (slot, offset) on station st right now; NULL if no schedule */
static const ep_t *live(int st, int64_t *off_ms, int64_t *slot_t0) {
    if (!sch || st < 0 || st >= sch->n) return NULL;
    int slot; int64_t off;
    int64_t now = wall_ms();
    if (sched_live(sch, st, now, &slot, &off)) return NULL;
    if (off_ms) *off_ms = off;
    if (slot_t0) *slot_t0 = now - off;
    return &sch->st[st].eps[slot];
}

const ep_t *player_episode(void) { return live(tuned, NULL, NULL); }

static void stop_stream(void) {
    if (stream) { hs_close(stream); stream = NULL; }
    if (dec) { dec_free(dec); dec = NULL; }
    s_head = s_len = 0;
    s_eof = s_fed_eof = s_fails = 0;
}

static void stop_all(void) {
    stop_stream();
    pj_reset(&job);
    audio_play(0);
    audio_flush();
    play_head_ms = -1;
}

/* This episode can't play here: say why, and wait in the gap for the next one. */
static void unsupported(const char *why) {
    stop_stream();
    stats.unsupported++;
    state = PL_UNSUPPORTED;
    snprintf(unsupported_why, sizeof unsupported_why, "%s", why);
    set_status(why);
    audio_hiss(0.3f);
    sub = S_GAP;
}

void player_set_schedule(const sched_t *s) {
    sch = s;
    if (!s) { stop_all(); pj_reset(&warm); sub = S_IDLE; }
    else if (tuned >= s->n) tuned = s->n - 1;
}

void player_tune(int st) {
    if (st == tuned && sub != S_IDLE) return;
    stop_all();
    pj_reset(&warm);
    tuned = st;
    sub = S_IDLE;
    fails = 0;
    tune_t0 = tick_ms();
    tune_counted = 0;
    settled_at = tick_ms();
    warm_idx = 0;
    if (st < 0) { state = PL_OFF; audio_hiss(0); return; }
    stats.tunes++;
    state = PL_TUNING;
    set_status("Tuning");
    audio_hiss(0.8f);
}

static int classify(int fmt) {
    if (fmt == FMT_MP3) return 0;
    if (fmt == FMT_WAV) return 3;
    return 1;   /* AAC: refined to HE-AAC once the decoder reports SBR */
}

/* Bytes per second of audio, to size the buffer and the ranges. */
static float byte_rate(const probe_t *p) {
    if (p->fmt == FMT_MP3 && p->mp3.byte_rate > 0) return (float)p->mp3.byte_rate;
    if (p->fmt == FMT_MP3 && p->mp3.bitrate_kbps > 0) return p->mp3.bitrate_kbps * 125.0f;
    if (p->fmt == FMT_WAV) return (float)p->wav_rate * p->wav_ch * 2;
    if (p->total > 0 && audio_end_ms > 0) return (float)p->total * 1000.0f / (float)audio_end_ms;
    return 24000;                                    /* 192 kbps */
}

/* Ask for the next range once a whole one fits in the buffer, or sooner, for
 * what fits, once less than LOW_S is left: a new connection can take 8 s on a
 * Playdate, and a 320 kbps show only has room for about 26 s. */
static void fetch_next(void) {
    if (stream || s_eof || (int32_t)(tick_ms() - s_retry_at) < 0) return;
    int64_t want = s_chunk;
    if (s_total > 0 && s_total - s_next < want) want = s_total - s_next;
    if (want <= 0) { s_eof = 1; return; }
    int room = s_cap - s_len;
    if (room < want) {
        if (s_len >= s_rate * LOW_S || room < s_chunk / 4) return;
        want = room;
    }
    if (dbg_spoil_link && sub == S_PLAY) {           /* test: the link stops working */
        dbg_spoil_link = 0;
        char *path = strstr(s_url, "://");
        path = path ? strchr(path + 3, '/') : NULL;
        if (path) snprintf(path, sizeof s_url - (size_t)(path - s_url), "/static-test-expired-link.mp3");
        LOG("player: test: spoiling the stream's link: %.80s", s_url);
    }
    stream = hs_open(s_url, "GET", s_next, s_next + want - 1, NULL, 0, NULL);
    hs_set_priority(stream, NET_PRIO_HIGH);
    s_req_from = s_next; s_req_len = want;
}

/* Where to read from for media time `at`: the file byte, the media time that
 * byte starts (exact for MP4, estimated for MP3), and for MP4 the sample.
 * -1: the MP4 index isn't loaded; -2: past the end of the MP4. */
static int locate(const probe_t *p, int64_t at, int64_t *byte, int64_t *start_ms, uint32_t *sample) {
    *byte = 0; *start_ms = at; *sample = 0;
    if (p->fmt == FMT_MP3) {
        *byte = at > 500 ? mp3_byte_for_ms(&p->mp3, at) : p->mp3.audio_start;
        if (at <= 500) *start_ms = 0;
    } else if (p->fmt == FMT_WAV) {
        *byte = p->wav_data + (at * p->wav_rate / 1000) * 2LL * p->wav_ch;
    } else if (p->fmt == FMT_MP4) {
        if (!moov_cache.ok || moov_cache.key != hash(ep_url)) return -1;
        *sample = mp4_sample_for_ms(&moov_cache.idx, at, start_ms);
        mp4iter_t it;
        if (mp4_iter_seek(&moov_cache.idx, *sample, &it)) return -2;
        *byte = (int64_t)it.off;
    }
    return 0;
}

static dec_t *new_decoder(int64_t byte, int64_t start_ms, uint32_t sample) {
    const probe_t *p = &s_probe;
    dec_t *d = dec_new(p->fmt, byte, start_ms, p->fmt == FMT_MP4 ? &moov_cache.idx : NULL, sample, p->wav_rate, p->wav_ch);
    if (d) dec_set_gain_db(d, ep_gain);
    return d;
}

/* open the stream for the live episode at the live offset (+ expected latency) */
static void start_stream(const probe_t *p) {
    int64_t off;
    const ep_t *ep = live(tuned, &off, NULL);
    if (!ep) return;
    int64_t byte, start_ms;
    uint32_t first_sample;
    int where = locate(p, off + ttfb_avg, &byte, &start_ms, &first_sample);
    if (where == -1) { sub = S_IDLE; return; }                         /* re-probe for the MP4 index */
    if (where == -2) { audio_end_ms = 0; sub = S_GAP; return; }
    if (p->total > 0 && byte >= p->total) { sub = S_GAP; return; }   /* past the end of the file */
    s_probe = *p;
    s_probe.final = NULL;
    dec = new_decoder(byte, start_ms, first_sample);
    if (!dec) { set_status("Decoder out of memory"); sub = S_WAIT; retry_at = tick_ms() + 5000; return; }
    fmt_class = classify(p->fmt);
    stream_audio_s = stream_decode_s = 0;
    if (!sbuf) {                                     /* once, and kept: big blocks fragment the heap */
        sbuf_max = dbg_ring_max && dbg_ring_max < SBUF_MAX ? dbg_ring_max : SBUF_MAX;
        for (; sbuf_max >= SBUF_MIN && !(sbuf = malloc((size_t)sbuf_max)); sbuf_max -= SBUF_MIN) {}
        if (!sbuf) { dec_free(dec); dec = NULL; set_status("Out of memory"); sub = S_WAIT; retry_at = tick_ms() + 5000; return; }
        LOG("stream buffer: %d KB", sbuf_max / 1024);
    }
    s_rate = byte_rate(p);
    s_cap = (int)fminf(s_rate * BUF_S, (float)sbuf_max);
    if (s_cap < SBUF_MIN) s_cap = sbuf_max < SBUF_MIN ? sbuf_max : SBUF_MIN;
    s_chunk = (int)fminf(s_rate * CHUNK_S, (float)(s_cap / 2));
    s_head = s_len = 0;
    s_eof = s_fed_eof = s_fails = 0;
    s_next = byte;
    s_total = p->total > 0 ? p->total : -1;
    s_retry_at = tick_ms();
    snprintf(s_url, sizeof s_url, "%s", p->final);
    fetch_next();
    sub = S_CONNECT;
    play_head_ms = -1;
    /* Audio decoded for a stream that broke before it played would fill the
     * ring, and this one could never queue its first sample. */
    if (!audio_playing()) audio_flush();
}

static void begin_episode(void) {
    int64_t off, t0;
    const ep_t *ep = live(tuned, &off, &t0);
    stop_stream();
    if (!ep) { set_status("No schedule yet"); state = PL_TROUBLE; sub = S_WAIT; retry_at = tick_ms() + 2000; return; }
    snprintf(ep_url, sizeof ep_url, "%s", ep->url);
    slot_start_ms = t0;
    ep_gain = (float)ep->gain_db;
    audio_end_ms = (int64_t)ep->dur_s * 1000;
    if (ep->bulletin && audio_end_ms > BULLETIN_CAP_MS) audio_end_ms = BULLETIN_CAP_MS;
    if (off >= audio_end_ms) { sub = S_GAP; return; }       /* in the gap / past a bulletin's cap */
    probe_t *p = cache_find(ep_url);
    if (p && (p->fmt != FMT_MP4 || (moov_cache.ok && moov_cache.key == hash(ep_url)))) { start_stream(p); return; }
    pj_start(&job, ep_url, 1, NET_PRIO_HIGH);
    sub = S_PROBE;
    if (state != PL_PLAYING) set_status("Finding the episode");
}

static void trouble(const char *why, uint32_t retry_ms) {
    stop_stream();
    pj_reset(&job);
    fails++;
    if (fails > GIVE_UP_FAILS) {
        /* Don't loop "reconnecting" forever: say so plainly, keep trying slowly.
         * Turning the dial away and back tries again straight away. */
        char n[160]; net_describe(n, sizeof n);
        if (fails == GIVE_UP_FAILS + 1) LOG("player: %d tries failed (%s); now retrying once a minute (%s)", GIVE_UP_FAILS, why, n);
        why = net_wedged() ? "The network stopped answering. Reopening Static may help"
            : net_stuck() ? "The network stopped answering. Retrying" : "Can't reach this show. Trying again soon";
        retry_ms = GIVE_UP_RETRY_MS;
    }
    set_status(why);
    state = PL_TROUBLE;
    sub = S_WAIT;
    retry_at = tick_ms() + retry_ms;
    audio_hiss(0.35f);
}

/* drop decoded samples that are already in the past; returns samples kept (moved to the front) */
static int align_chunk(int n, int64_t live_ms, int64_t *chunk_ms) {
    int64_t p0 = *chunk_ms;
    int64_t end = p0 + (int64_t)n * 1000 / RATE;
    if (end <= live_ms) { *chunk_ms = end; return 0; }
    if (p0 >= live_ms) return n;
    int drop = (int)((live_ms - p0) * RATE / 1000);
    if (drop > n) drop = n;
    memmove(pcm, pcm + drop, (size_t)(n - drop) * sizeof pcm[0]);
    *chunk_ms = p0 + (int64_t)drop * 1000 / RATE;
    return n - drop;
}

/* The stream broke: reconnect with a growing pause (1.5, 3, 6, 12, 20 s).
 * From the second failure in a row, look the link up again from the feed's
 * URL, in case the redirect we cached went stale. */
static void stream_lost(const char *why, int http) {
    char n[160]; net_describe(n, sizeof n);
    stats.stream_fail++;
    LOG("stream failed: %s (HTTP %d), try %d; %s", why, http, fails + 1, n);
    probe_t *p = cache_find(ep_url);
    if (fails >= 1 && p && p->fmt != FMT_MP4) cache_drop(ep_url);   /* an MP4 keeps its index */
    uint32_t wait = 1500u << (fails < 4 ? fails : 4);
    if (wait > 20000) wait = 20000;
    trouble("Lost the stream, reconnecting", wait);
}

/* A range failed. While the buffer still has a while to play, try it again
 * quietly; otherwise reconnect at the live position as before. */
static void range_failed(const char *why, int http) {
    if (http == 403 || http == 404 || http == 410) cache_drop(ep_url);   /* stale signed link */
    float ahead_s = s_len / s_rate + (float)audio_buffered() / RATE;
    if (sub == S_PLAY && http < 400 && ahead_s > 10 && s_fails < CHUNK_RETRIES) {
        s_fails++;
        hs_close(stream); stream = NULL;
        uint32_t wait = 2000u << s_fails;
        s_retry_at = tick_ms() + wait;
        char n[160]; net_describe(n, sizeof n);
        LOG("stream: range failed (%s, HTTP %d), %.0f s still buffered; again in %u s; %s", why, http, (double)ahead_s, (unsigned)wait / 1000, n);
        return;
    }
    stream_lost(why, http);
}

/* the range is all here: close it, so other requests can have the network
 * unless the next range is due now */
static void range_done(void) {
    int64_t t = hs_total(stream);
    if (t > 0) s_total = t;
    int short_body = s_next - s_req_from < s_req_len;
    snprintf(s_url, sizeof s_url, "%s", hs_url(stream));   /* where the redirects ended */
    hs_close(stream); stream = NULL;
    s_fails = 0;
    if (short_body || (s_total > 0 && s_next >= s_total)) s_eof = 1;
    fetch_next();   /* ask now, while the connection is still kept for its server */
}

/* Playback is behind live by more than SKIP_MS: a tune's first bytes arrive
 * seconds after the range was asked for, and an underrun leaves it behind too.
 * Skip the bytes that are already in the past instead of decoding them: on a
 * Playdate decoding runs only a little faster than real time, so catching up
 * that way took 20-30 s after tuning and stretched every dropout. Returns 0
 * while the bytes for `target` are still to come. */
static int catch_up(int64_t target) {
    int64_t byte, start_ms;
    uint32_t sample;
    if (locate(&s_probe, target, &byte, &start_ms, &sample)) return 1;   /* decode the rest of the way */
    int64_t oldest = s_next - s_len;                 /* file offset of the oldest byte in the buffer */
    if (byte <= oldest) return 1;
    if (byte > s_next) {                             /* not here yet: drop what we have */
        s_head = s_len = 0;
        if (!stream) s_next = byte;
        return 0;
    }
    int drop = (int)(byte - oldest);
    s_head = (s_head + drop) % s_cap;
    s_len -= drop;
    int64_t was = dec_pos_ms(dec);
    int lite = dec_is_lite(dec);
    dec_free(dec);
    dec = new_decoder(byte, start_ms, sample);
    if (!dec) { trouble("Decoder out of memory", 5000); return 0; }
    if (lite) dec_set_lite(dec);
    if (sub == S_PLAY) stats.resyncs++;
    LOG("skipped %.1f s to catch up with live", (double)(start_ms - was) / 1000);
    return 1;
}

static void pump_stream(float budget_s) {
    if (!dec) return;
    /* network -> stream buffer: everything that has arrived */
    fetch_next();
    if (stream) {
        hs_poll(stream);
        int s = hs_state(stream);
        if (s == HS_FAIL) { range_failed(hs_error(stream), hs_status(stream)); return; }
        if (s == HS_BODY || s == HS_DONE) {
            int st = hs_status(stream);
            if (st == 416) {                             /* asked past the end */
                hs_close(stream); stream = NULL; s_eof = 1;
                if (sub == S_CONNECT) { stop_stream(); sub = S_GAP; return; }   /* file shorter than scheduled */
            } else if (st >= 400 && s_len > 0 && strcmp(s_url, ep_url) && (st == 401 || st == 403 || st == 404 || st == 410)) {
                /* a signed link expired (SoundCloud's last a few minutes):
                 * follow the episode's own link again for a fresh one */
                LOG("stream: link refused (HTTP %d); following the episode's link again", st);
                cache_drop(ep_url);
                hs_close(stream); stream = NULL;
                snprintf(s_url, sizeof s_url, "%s", ep_url);
                return;
            } else if (st >= 400) {
                LOG("stream: server said %d", st);
                cache_drop(ep_url);
                char m[48]; snprintf(m, sizeof m, "Show's server said %d", st);
                trouble(m, 3000);
                return;
            } else if (s_total > 0 && hs_total(stream) > 0 && hs_total(stream) != s_total) {
                /* a new link led to a different file (ads stitched in): start over at live */
                cache_drop(ep_url);
                stream_lost("file changed size", st);
                return;
            } else if (hs_range_start(stream) != s_req_from) {
                /* the server ignored the range: these bytes aren't the ones we need */
                char m[48]; snprintf(m, sizeof m, "server sent bytes from %lld", (long long)hs_range_start(stream));
                cache_drop(ep_url);
                stream_lost(m, st);
                return;
            }
        }
        if (stream && (s == HS_BODY || s == HS_DONE)) {
            for (int guard = 0; guard < 32; guard++) {
                int room = s_cap - s_len;
                if (room <= 0) break;
                int tail = (s_head + s_len) % s_cap;
                int n = hs_read(stream, sbuf + tail, s_cap - tail < room ? s_cap - tail : room);
                if (n > 0) {
                    if (sub == S_CONNECT) {
                        uint32_t t = hs_ttfb_ms(stream);
                        if (t) ttfb_avg = (ttfb_avg * 3 + t) / 4;
                        if (ttfb_avg > 4000) ttfb_avg = 4000;
                        sub = S_ALIGN;
                    }
                    s_len += n; s_next += n;
                    continue;
                }
                if (n == -1) { range_done(); break; }
                if (n == -2) { range_failed(hs_error(stream), hs_status(stream)); return; }
                break;
            }
        }
    }
    /* stream buffer -> decoder input, skipping what's already in the past */
    int64_t live_off = wall_ms() - slot_start_ms;
    int64_t want_ms = -1;
    if (sub == S_ALIGN && play_head_ms < 0) want_ms = live_off + PREROLL_MS;
    else if (sub == S_PLAY) want_ms = live_off + audio_buffered() * 1000LL / RATE;
    if (want_ms >= 0 && want_ms - dec_pos_ms(dec) > SKIP_MS && !catch_up(want_ms)) return;
    if (!dec) return;
    while (s_len > 0) {
        int room = dec_space(dec);
        if (room < 1024) break;
        int run = s_cap - s_head < s_len ? s_cap - s_head : s_len;
        if (run > room) run = room;
        dec_push(dec, sbuf + s_head, run);
        s_head = (s_head + run) % s_cap;
        s_len -= run;
    }
    if (s_eof && !s_len && !stream && !s_fed_eof) { dec_end_input(dec); s_fed_eof = 1; }

    /* decoder -> ring, inside the frame's CPU budget */
    while (cpu_s() < budget_s) {
        int buffered_ms = audio_buffered() * 1000 / RATE;
        if ((sub == S_PLAY || play_head_ms >= 0) && buffered_ms >= TARGET_MS) break;
        if (audio_space() < 8192) break;
        int64_t chunk_ms = dec_pos_ms(dec);
        if (chunk_ms >= audio_end_ms) { stop_stream(); sub = S_GAP; break; }
        float t0 = cpu_s();
        int n = dec_step(dec, pcm, (int)(sizeof pcm / sizeof pcm[0]));
        float dt = cpu_s() - t0;
        if (slow_decode > 1 && n > 0) dt = stretch_decode(t0, dt, n);
        if (n == -1) break;                              /* waiting for bytes */
        if (n == -2) {                                   /* end of file (or broken) */
            int fr = dec_frames(dec);
            stop_stream();
            if (fr == 0 && sub != S_PLAY) unsupported("Can't play this one");
            sub = S_GAP;
            break;
        }
        if (dec_is_sbr(dec)) fmt_class = 2;
        stats.decode_s[fmt_class] += dt;
        stats.audio_s[fmt_class] += (float)n / RATE;
        stream_decode_s += dt;
        stream_audio_s += (float)n / RATE;
        if (n == 0) continue;
        stats.frames[fmt_class]++;
        if (chunk_ms + (int64_t)n * 1000 / RATE > audio_end_ms) n = (int)((audio_end_ms - chunk_ms) * RATE / 1000);
        if (n <= 0) continue;

        if (sub == S_ALIGN) {
            /* land on the true live position: drop what's already past,
             * then queue PREROLL and start when the clock reaches it */
            int64_t want = live_off + PREROLL_MS;
            if (play_head_ms < 0) {
                n = align_chunk(n, want, &chunk_ms);
                if (n == 0) continue;
                play_head_ms = chunk_ms;
            }
            audio_push(pcm, n);
            continue;
        }
        if (sub == S_PLAY) {
            /* keep sync: compare the ring's head against the live clock */
            int64_t head = chunk_ms - audio_buffered() * 1000LL / RATE;
            drift = head - live_off;
            static int correcting;
            if (drift < -RESYNC_MS) {                   /* behind: skip ahead */
                int64_t keep_from = chunk_ms - drift;
                int64_t cms = chunk_ms;
                n = align_chunk(n, keep_from, &cms);
                if (!correcting) { correcting = 1; stats.resyncs++; }
                if (n == 0) continue;
            } else if (drift > -RESYNC_MS / 2) correcting = 0;
            if (drift > RESYNC_MS && audio_space() > (int)(drift * RATE / 1000) + n) {   /* ahead: pad silence */
                static const int16_t zeros[1024];
                int pad = (int)(drift * RATE / 1000);
                while (pad > 0) { int k = pad > 1024 ? 1024 : pad; audio_push(zeros, k); pad -= k; }
                stats.resyncs++;
            }
            audio_push(pcm, n);
        }
    }

    /* Too slow to keep up on this Playdate: try lite mode, then give up on this episode */
    if (stream_audio_s >= JUDGE_S) {
        float r = stream_decode_s / stream_audio_s;
        if (r > LITE_ABOVE && !dec_is_lite(dec) && dec_set_lite(dec)) {
            LOG("decoding takes %.0f%% of real time: lite mode", (double)(r * 100));
            stats.lite++;
            stream_audio_s = stream_decode_s = 0;
        } else if (sub == S_ALIGN && r > HEAVY_ABOVE) {
            LOG("decoding takes %.0f%% of real time%s: too heavy", (double)(r * 100), dec_is_lite(dec) ? " in lite mode" : "");
            unsupported("Too heavy to play");
            return;
        }
    }
    if (sub == S_ALIGN && play_head_ms >= 0) {
        int64_t now_off = wall_ms() - slot_start_ms;
        if (now_off >= play_head_ms) {                   /* the clock reached the first queued sample */
            if (now_off - play_head_ms > RESYNC_MS) { audio_flush(); play_head_ms = -1; return; }   /* too late: re-align */
            sub = S_PLAY;
            state = PL_PLAYING;
            playing_since = tick_ms();
            set_status("");
            audio_hiss(0);
            audio_play(1);
            fails = 0;
            listen_mark = tick_ms();
            if (!tune_counted) {
                uint32_t t = tick_ms() - tune_t0;
                tune_counted = 1;
                stats.tune_ok++;
                stats.tune_ms_sum += t;
                if (t > stats.tune_ms_max) stats.tune_ms_max = t;
                LOG("first sound %u ms after tuning (fmt class %d)", (unsigned)t, fmt_class);
            }
        } else if (play_head_ms - now_off > 3 * TARGET_MS) {
            /* far ahead of live (the clock changed): start again from now */
            stop_stream(); audio_flush(); play_head_ms = -1;
            sub = S_IDLE;
        }
    }
}

static void maybe_warm(void) {
    /* one background probe at a time, only while playback is comfortable */
    if (warm.st != PJ_IDLE) {
        if (pj_poll(&warm)) {
            /* a host that doesn't answer ties up the network for everyone */
            if (warm.st == PJ_FAIL) warm_rest_until = tick_ms() + WARM_REST_MS;
            pj_reset(&warm);
        }
        return;
    }
    if ((int32_t)(tick_ms() - warm_rest_until) < 0) return;
    if (sub != S_PLAY || audio_buffered() < RATE * 2 || job.st != PJ_IDLE || player_hungry()) return;
    if (!sch || tuned < 0) return;
    int64_t now = wall_ms();
    /* next episode on this station, close to the end of this one */
    int slot; int64_t off;
    if (!sched_live(sch, tuned, now, &slot, &off)) {
        int64_t rem = sched_slot_remaining(sch, tuned, now);
        if (rem < WARM_AHEAD_S * 1000) {
            const station_t *s = &sch->st[tuned];
            const ep_t *next = &s->eps[(slot + 1) % s->n];
            if (!cache_find(next->url)) { pj_start(&warm, next->url, 0, NET_PRIO_LOW); return; }
        }
    }
    /* the neighbors' live episodes, after 10 s on this station */
    if (tick_ms() - settled_at < 10000) return;
    static const int order[] = { 1, -1, 2, -2 };
    while (warm_idx < 4) {
        int st = tuned + order[warm_idx++];
        if (st < 0 || st >= sch->n) continue;
        const ep_t *ep = live(st, NULL, NULL);
        if (ep && !cache_find(ep->url)) { pj_start(&warm, ep->url, 0, NET_PRIO_LOW); return; }
    }
}

void player_update(float budget_s) {
    if (state != PL_OFF && sub == S_PLAY && tick_ms() - listen_mark >= 1000) {
        stats.listen_s += (tick_ms() - listen_mark) / 1000;
        listen_mark += ((tick_ms() - listen_mark) / 1000) * 1000;
    }
    if (tuned < 0 || !sch) return;

    /* the live episode changed (or we're idle): start it */
    int64_t off, t0;
    const ep_t *ep = live(tuned, &off, &t0);
    if (ep && sub != S_IDLE && sub != S_WAIT) {
        int same = !strcmp(ep->url, ep_url) && (t0 - slot_start_ms < 2500 && slot_start_ms - t0 < 2500);
        if (!same) {
            if (sub == S_PLAY) audio_expect_gap();
            stop_all();
            sub = S_IDLE;
            tune_t0 = tick_ms();
        }
    }

    switch (sub) {
    case S_IDLE:
        begin_episode();
        break;
    case S_PROBE:
        if (pj_poll(&job)) {
            if (job.st == PJ_DONE) {
                probe_t p = job.p;
                p.final = job.p.final;
                start_stream(&p);
                if (job.p.moov_size > 0) {
                    int kb = (int)(job.p.moov_size / 1024);
                    uint32_t ms = tick_ms() - job.t0;
                    if (kb > stats.moov_kb_max) stats.moov_kb_max = kb;
                    if (ms > stats.moov_ms_max) stats.moov_ms_max = ms;
                }
                job.p.final = NULL;                  /* start_stream copied what it needed */
                free(p.final);
                pj_reset(&job);
            } else {
                char m[64];
                snprintf(m, sizeof m, "Can't reach this show (%.36s)", job.err);
                if (!strcmp(job.err, "not AAC") || !strcmp(job.err, "unknown format") || !strcmp(job.err, "WAV format")) {
                    pj_reset(&job);
                    unsupported("Can't play this one");
                } else {
                    stats.tune_fail += !tune_counted && fails == 0;
                    trouble(m, fails < 3 ? 3000 : 10000);
                }
            }
        }
        break;
    case S_CONNECT:
    case S_ALIGN:
    case S_PLAY:
        pump_stream(budget_s);
        if (sub == S_CONNECT && stream && tick_ms() - hs_started(stream) > 1500 && state != PL_PLAYING) set_status("Connecting");
        if (sub == S_ALIGN && state != PL_PLAYING) set_status("Buffering");
        break;
    case S_GAP:
        /* between episodes / past a bulletin's cap: wait for the next slot */
        if (state == PL_PLAYING || state == PL_TUNING) {
            if (audio_buffered() == 0) audio_expect_gap();
            if (state == PL_TUNING) { state = PL_GAP; set_status("Next show in a moment"); audio_hiss(0.3f); }
        }
        if (state == PL_UNSUPPORTED) {
            int min = (int)((sched_slot_remaining(sch, tuned, wall_ms()) + 59999) / 60000);
            if (min > 1) snprintf(status, sizeof status, "%s. Next show in %d min", unsupported_why, min);
            else snprintf(status, sizeof status, "%s. Next show in a moment", unsupported_why);
        }
        break;
    case S_WAIT:
        if (tick_ms() >= retry_at) {
            stats.reconnects++;
            sub = S_IDLE;
            if (state == PL_TROUBLE) state = PL_TUNING;
        }
        break;
    }
    maybe_warm();
}
