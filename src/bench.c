#include "common.h"
#include "bench.h"
#include "dec.h"
#include "net.h"

#define STREAM_BYTES (256 * 1024)
#define STREAM_FROM 1000000

static const struct { const char *path, *name; int fmt; } k_clips[BENCH_CLIPS] = {
    { "clips/mp3_128k.mp3", "MP3 128k stereo", FMT_MP3 },
    { "clips/heaac_64k.aac", "HE-AAC 64k stereo", FMT_ADTS },
    { "clips/aaclc_128k.aac", "AAC-LC 128k stereo", FMT_ADTS },
};

static const char *k_steps[BENCH_STEPS] = {
    "Decode MP3", "Decode HE-AAC", "Decode AAC-LC", "Fetch today's schedule", "Stream live episodes",
};
const char *bench_step_name(int step) { return step >= 0 && step < BENCH_STEPS ? k_steps[step] : ""; }

/* per-step working state */
static uint8_t *clip; static int clip_len, clip_off;
static dec_t *d;
static int16_t out[8192];
static fetch_t sf;
static uint32_t t0;
static hs_t *sh;
static int si;
static sched_t *sched_tmp;
static const char *stream_urls[BENCH_STREAMS];

static uint8_t *load(const char *path, int *len) {
    SDFile *f = pd->file->open(path, kFileRead);
    if (!f) return NULL;
    pd->file->seek(f, 0, SEEK_END);
    int n = pd->file->tell(f);
    pd->file->seek(f, 0, SEEK_SET);
    uint8_t *b = n > 0 ? malloc((size_t)n) : NULL;
    if (b && pd->file->read(f, b, (unsigned)n) != n) { free(b); b = NULL; }
    pd->file->close(f);
    if (b) *len = n;
    return b;
}

static void begin_step(bench_t *b) {
    t0 = tick_ms();
    if (b->step < BENCH_CLIPS) {
        const int i = b->step;
        b->clip[i].name = k_clips[i].name;
        free(clip); clip = load(k_clips[i].path, &clip_len); clip_off = 0;
        if (d) { dec_free(d); d = NULL; }
        if (clip) d = dec_new(k_clips[i].fmt, 0, 0, NULL, 0, 0, 0);
        if (!clip || !d) { LOG("bench: can't load %s", k_clips[i].path); b->step++; begin_step(b); }
        LOG("bench: %s", k_steps[b->step]);
    } else if (b->step == BENCH_CLIPS) {
        free(clip); clip = NULL;
        if (d) { dec_free(d); d = NULL; }
        fetch_start(&sf, SCHEDULE_URL, "GET", NULL, 0, NULL, 2 * 1024 * 1024);
        LOG("bench: fetching the schedule");
    } else if (b->step == BENCH_CLIPS + 1) {
        si = 0;
        b->nstreams = 0;
        /* three MP3 stations and SHEILA (MP4): the live episode on each */
        if (sched_tmp) {
            int64_t now = wall_ms();
            int mp3s = 0;
            for (int s = 0; s < sched_tmp->n && b->nstreams < BENCH_STREAMS; s++) {
                int slot; int64_t off;
                if (sched_live(sched_tmp, s, now, &slot, &off)) continue;
                const ep_t *ep = &sched_tmp->st[s].eps[slot];
                const char *q = strchr(ep->url, '?');
                size_t l = q ? (size_t)(q - ep->url) : strlen(ep->url);
                int mp4 = l > 4 && (!strncasecmp(ep->url + l - 4, ".mp4", 4) || !strncasecmp(ep->url + l - 4, ".m4a", 4));
                if (!mp4 && mp3s >= 3) continue;
                if (mp4 && strcmp(sched_tmp->st[s].id, "sheila_01")) continue;
                if (!mp4) mp3s++;
                bench_stream_t *bs = &b->stream[b->nstreams];
                snprintf(bs->station, sizeof bs->station, "%s", sched_tmp->st[s].id);
                bs->fmt_mp4 = mp4;
                stream_urls[b->nstreams++] = ep->url;
            }
        }
        if (b->nstreams == 0) { b->step++; b->done = 1; }
        LOG("bench: streaming %d live episodes", b->nstreams);
    } else b->done = 1;
}

void bench_start(bench_t *b) {
    memset(b, 0, sizeof *b);
    b->step = 0;
    begin_step(b);
}

static void next(bench_t *b) { b->step++; begin_step(b); }

void bench_update(bench_t *b, float budget_s, sched_t **sched_out) {
    if (b->done) return;
    if (b->step < BENCH_CLIPS) {
        bench_clip_t *c = &b->clip[b->step];
        while (cpu_s() < budget_s) {
            if (clip_off < clip_len && dec_space(d) > 4096) {
                int k = clip_len - clip_off;
                if (k > dec_space(d)) k = dec_space(d);
                dec_push(d, clip + clip_off, k);
                clip_off += k;
                if (clip_off >= clip_len) dec_end_input(d);
            }
            float s = cpu_s();
            int n = dec_step(d, out, (int)(sizeof out / sizeof out[0]));
            float dt = cpu_s() - s;
            c->decode_s += dt;
            if (n > 0) { c->audio_s += (float)n / RATE; c->frames++; }
            if (n == -2 || (n == -1 && clip_off >= clip_len)) {
                c->errors = dec_errors(d);
                c->ok = c->frames > 0;
                LOG("bench: %s: %.2f s CPU for %.2f s audio (%.1f%%), %d frames, %d errors",
                    c->name, (double)c->decode_s, (double)c->audio_s,
                    c->audio_s > 0 ? (double)(c->decode_s / c->audio_s * 100) : 0.0, c->frames, c->errors);
                next(b);
                return;
            }
        }
        return;
    }
    if (b->step == BENCH_CLIPS) {
        if (!fetch_poll(&sf)) {
            if (tick_ms() - t0 > 30000) { fetch_free(&sf); LOG("bench: schedule timed out"); next(b); }
            return;
        }
        b->sched_ms = tick_ms() - t0;
        clock_note_http_date(sf.date);
        if (!b->clock_known) { int64_t off; b->clock_known = clock_offset(&off); b->clock_off_ms = off; }
        b->sched_kb = sf.len / 1024;
        if (sf.done == 1 && sf.status == 200) {
            char *text = sf.buf; sf.buf = NULL;
            sched_tmp = sched_parse(text);
            b->sched_ok = sched_tmp != NULL;
        }
        LOG("bench: schedule %s, %d KB in %u ms", b->sched_ok ? "ok" : "FAILED", b->sched_kb, (unsigned)b->sched_ms);
        fetch_free(&sf);
        next(b);
        return;
    }
    if (b->step == BENCH_CLIPS + 1) {
        bench_stream_t *bs = &b->stream[si];
        if (!sh) {
            sh = hs_open(stream_urls[si], "GET", STREAM_FROM, STREAM_FROM + STREAM_BYTES - 1, NULL, 0, NULL);
            t0 = tick_ms();
            return;
        }
        hs_poll(sh);
        static uint8_t buf[8192];
        int finished = 0;
        for (int g = 0; g < 32; g++) {
            int n = hs_read(sh, buf, sizeof buf);
            if (n > 0) { bs->bytes += n; if (bs->bytes >= STREAM_BYTES) { finished = 1; break; } continue; }
            if (n < 0) { finished = 1; if (n == -2) snprintf(bs->err, sizeof bs->err, "%s", hs_error(sh)); }
            break;
        }
        if (!finished && tick_ms() - t0 > 30000) { finished = 1; snprintf(bs->err, sizeof bs->err, "timeout"); }
        if (finished) {
            bs->status = hs_status(sh);
            bs->hops = hs_hops(sh);
            bs->ttfb_ms = hs_ttfb_ms(sh);
            bs->ms = tick_ms() - t0;
            uint32_t body_ms = bs->ms > bs->ttfb_ms ? bs->ms - bs->ttfb_ms : 1;
            bs->kbps = bs->bytes / 1.024f / (float)body_ms;
            LOG("bench: %s: HTTP %d, %d hops, first byte %u ms, %d KB at %.1f KB/s %s",
                bs->station, bs->status, bs->hops, (unsigned)bs->ttfb_ms, bs->bytes / 1024, (double)bs->kbps, bs->err);
            hs_close(sh); sh = NULL;
            if (++si >= b->nstreams) {
                b->done = 1;
                b->step = BENCH_STEPS;
                if (sched_out) { *sched_out = sched_tmp; sched_tmp = NULL; }
            }
        }
    }
}
