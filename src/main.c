/* Static for Playdate (preview): a two-minute test that reports how well this
 * Playdate decodes and streams, then the radio itself, with the crank as the
 * dial. Everything streams straight from each show's own host. */
#include "common.h"
#include "audio.h"
#include "bench.h"
#include "net.h"
#include "player.h"
#include "report.h"
#include "sched.h"
#include "faces.h"
#include "third_party/qrcodegen.h"
#include <math.h>

PlaydateAPI *pd;

/* ---------- small platform helpers ---------- */

static SDFile *log_file;
static int64_t clock_off_ms;     /* device minus true */
static int clock_known;

void logf_(const char *fmt, ...) {
    char line[600];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(line, sizeof line, fmt, ap);
    va_end(ap);
    pd->system->logToConsole("%s", line);
    if (log_file) {
        char ts[24];
        int n = snprintf(ts, sizeof ts, "[%u] ", (unsigned)tick_ms());
        pd->file->write(log_file, ts, (unsigned)n);
        pd->file->write(log_file, line, (unsigned)strlen(line));
        pd->file->write(log_file, "\n", 1);
        pd->file->flush(log_file);
    }
}

static int64_t device_unix_ms(void) {
    unsigned ms = 0;
    unsigned s = pd->system->getSecondsSinceEpoch(&ms);
    return ((int64_t)s + 946684800LL) * 1000 + ms;
}
int64_t wall_ms(void) { return device_unix_ms() - clock_off_ms; }
int clock_offset(int64_t *off) { *off = clock_off_ms; return clock_known; }
void clock_set_true_unix_s(int64_t unix_s, const char *source) {
    int64_t dev = device_unix_ms();
    int64_t off = dev - unix_s * 1000;
    /* the server only gives whole seconds: keep the device's milliseconds
     * unless it's clearly wrong */
    clock_off_ms = (off > 1500 || off < -1500) ? off : 0;
    clock_known = 1;
    LOG("clock: device is %+lld ms from %s time (using %+lld)", (long long)off, source, (long long)clock_off_ms);
}

static int clock_from_server;
void clock_note_http_date(const char *date) {
    if (clock_from_server || !date || !*date) return;
    static const char *mons = "JanFebMarAprMayJunJulAugSepOctNovDec";
    char mon[4] = "";
    int d, y, h, mi, s;
    if (sscanf(date, "%*3s, %d %3s %d %d:%d:%d", &d, mon, &y, &h, &mi, &s) != 6) return;
    const char *m = strstr(mons, mon);
    if (!m) return;
    clock_set_true_unix_s(unix_from_utc(y, (int)(m - mons) / 3 + 1, d, h, mi, s), "HTTP Date");
}

int64_t unix_from_utc(int y, int mo, int d, int h, int mi, int s) {
    y -= mo <= 2;
    int64_t era = (y >= 0 ? y : y - 399) / 400;
    int64_t yoe = y - era * 400;
    int64_t doy = (153 * (mo + (mo > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    int64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    int64_t days = era * 146097 + doe - 719468;
    return days * 86400 + h * 3600 + mi * 60 + s;
}

int data_write(const char *name, const void *buf, int len) {
    SDFile *f = pd->file->open(name, kFileWrite);
    if (!f) return -1;
    int w = pd->file->write(f, buf, (unsigned)len);
    pd->file->close(f);
    return w == len ? 0 : -1;
}

char *data_read(const char *name, int *len) {
    SDFile *f = pd->file->open(name, kFileReadData);
    if (!f) return NULL;
    pd->file->seek(f, 0, SEEK_END);
    int n = pd->file->tell(f);
    pd->file->seek(f, 0, SEEK_SET);
    char *b = malloc((size_t)(n > 0 ? n : 0) + 1);
    if (!b) { pd->file->close(f); return NULL; }
    int r = n > 0 ? pd->file->read(f, b, (unsigned)n) : 0;
    pd->file->close(f);
    if (r < 0) r = 0;
    b[r] = 0;
    if (len) *len = r;
    return b;
}

/* ---------- app state ---------- */

enum { SC_INTRO, SC_NET, SC_NONET, SC_BENCH, SC_RESULTS, SC_RADIO };
static int screen = SC_INTRO;
static int after_net;            /* screen to go to once the network is up */
static LCDFont *f_big, *f_bold, *f_small;
static bench_t bench;
static sched_t *sched;
static fetch_t sched_fetch;
static int sched_fetching;
static uint32_t sched_at;
static int net_state;            /* 0 none, 1 enabling, 2 asking, 3 ready, -1 failed, -2 denied */
static char net_msg[96];
static int bench_done_before;
static int report_choice_made;

/* dial */
static float dial = 0;           /* in stations */
static float dial_target = -1;   /* d-pad animation target, -1 none */
static uint32_t crank_idle_at;
static int selected = -1;
static uint32_t selected_at;
static int view;                 /* 0 the dial, 1 episode info + QR, 2 stats */
static uint32_t last_draw;
static uint32_t rng_ui = 0xC0FFEE;
static char crash_note[64];
static PDMenuItem *mi_report, *mi_rerun;

/* Autopilot for simulator test runs (only when Data/autotest.txt exists):
 * runs the test, sends one report, then tunes across every station with the
 * sound muted, saving the screen every few seconds as Data/shot_NN.bin. */
static int autopilot;
static uint32_t ap_since, ap_shot_at;
static int ap_shot, ap_steps, ap_done;
static uint32_t app_start;
static int ap_radio_only;
/* "video:<station>,<station>,...": a scripted take for a demo video. Starts on
 * the first station, cranks to each next one, holds 3.5 s of sound on each,
 * then shows the episode info; records every frame (Data/vid_frames.bin:
 * u32 tick + 52x240 bytes) and the output audio (Data/vid_audio.raw). */
static int ap_video, vid_phase, vid_n, vid_i;
static char vid_ids[8][24];
static uint32_t vid_t, vid_t0, vid_last_tick;
static float ap_crank_change;
static SDFile *vid_f;
static void ap_save_screen(void) {
    uint8_t *fb = pd->graphics->getDisplayFrame();
    if (!fb || ap_shot >= 99) return;
    char name[32];
    snprintf(name, sizeof name, "shot_%02d.bin", ap_shot++);
    data_write(name, fb, LCD_ROWSIZE * LCD_ROWS);
}

void text_fold(const char *in, char *out, int cap);   /* textfold.c */

static void set_font(LCDFont *f) { pd->graphics->setFont(f); }
static int text_w(LCDFont *f, const char *s) { return pd->graphics->getTextWidth(f, s, strlen(s), kUTF8Encoding, 0); }
static void text(LCDFont *f, const char *s, int x, int y) { set_font(f); pd->graphics->drawText(s, strlen(s), kUTF8Encoding, x, y); }
static void text_c(LCDFont *f, const char *s, int cx, int y) { text(f, s, cx - text_w(f, s) / 2, y); }
static void text_box(LCDFont *f, const char *s, int x, int y, int w, int h) {
    set_font(f);
    pd->graphics->drawTextInRect(s, strlen(s), kUTF8Encoding, x, y, w, h, kWrapWord, kAlignTextLeft);
}
/* one line, shortened with an ellipsis to fit w */
static void text_fit(LCDFont *f, const char *s, int x, int y, int w) {
    char buf[200];
    snprintf(buf, sizeof buf, "%s", s);
    if (text_w(f, buf) <= w) { text(f, buf, x, y); return; }
    size_t n = strlen(buf);
    while (n > 0) {
        n--;
        while (n > 0 && ((unsigned char)buf[n] & 0xC0) == 0x80) n--;   /* UTF-8 boundary */
        buf[n] = 0;
        char t[210];
        snprintf(t, sizeof t, "%s…", buf);
        if (text_w(f, t) <= w) { text(f, t, x, y); return; }
    }
}

/* ---------- network bring-up ---------- */

static void on_access(bool allowed, void *ud) {
    (void)ud;
    net_state = allowed ? 3 : -2;
    LOG("network access %s", allowed ? "allowed" : "denied");
}

static void on_server_time(const char *t, const char *err) {
    if (t && *t) {
        long long s = atoll(t);
        if (s > 700000000LL) {
            clock_set_true_unix_s(s + 946684800LL, "Playdate server");
            clock_from_server = 1;
            bench.clock_known = 1;
            int64_t off; clock_offset(&off);
            bench.clock_off_ms = device_unix_ms() - (s + 946684800LL) * 1000;
        }
    } else LOG("server time: %s", err ? err : "?");
}

static void on_wifi(PDNetErr err) {
    if (err != NET_OK) { net_state = -1; snprintf(net_msg, sizeof net_msg, "Wi-Fi didn't come up (error %d)", err); LOG("%s", net_msg); return; }
    net_state = 2;
    enum accessReply r = pd->network->tcp->requestAccess(NULL, 0, true,
        "Static streams podcast audio straight from each show's own host, so it needs to reach many servers.", on_access, NULL);
    if (r == kAccessAllow) net_state = 3;
    else if (r == kAccessDeny) net_state = -2;
    if (net_state == 3) LOG("network access already allowed");
}

static void net_bring_up(int then) {
    after_net = then;
    screen = SC_NET;
    net_state = 1;
    net_msg[0] = 0;
    pd->network->setEnabled(true, on_wifi);
}

/* ---------- schedule ---------- */

static int station_index(const char *id) {
    if (!sched) return -1;
    for (int i = 0; i < sched->n; i++) if (!strcmp(sched->st[i].id, id)) return i;
    return -1;
}

static void adopt_schedule(sched_t *s) {
    int64_t age = wall_ms() - s->epoch_ms;
    if (age > 30LL * 3600 * 1000 || age < -30LL * 3600 * 1000) { LOG("schedule is for another day; ignoring"); sched_free(s); return; }
    char keep[24] = "";
    if (sched && selected >= 0 && selected < sched->n) snprintf(keep, sizeof keep, "%s", sched->st[selected].id);
    sched_t *old = sched;
    sched = s;
    player_set_schedule(sched);
    if (old) sched_free(old);
    int idx = keep[0] ? station_index(keep) : -1;
    if (idx < 0) {
        char *last = data_read("last.txt", NULL);
        if (last) { idx = station_index(last); free(last); }
    }
    if (idx < 0) idx = station_index("news_01");
    if (idx < 0) idx = 0;
    if (idx != selected) { selected = idx; dial = (float)idx; player_tune(idx); }
    sched_at = tick_ms();
    LOG("schedule: %d stations, generated %s", sched->n, sched->generated);
}

static void schedule_tick(void) {
    if (net_state != 3) return;
    if (!sched_fetching && (!sched || tick_ms() - sched_at > 15u * 60 * 1000)) {
        if (fetch_start(&sched_fetch, SCHEDULE_URL, "GET", NULL, 0, NULL, 2 * 1024 * 1024) == 0) sched_fetching = 1;
        sched_at = tick_ms();
    }
    if (sched_fetching && fetch_poll(&sched_fetch)) {
        sched_fetching = 0;
        clock_note_http_date(sched_fetch.date);
        if (sched_fetch.done == 1 && sched_fetch.status == 200) {
            char *text = sched_fetch.buf; sched_fetch.buf = NULL;
            sched_t *s = sched_parse(text);
            if (s) adopt_schedule(s);
        } else {
            LOG("schedule fetch failed (HTTP %d)", sched_fetch.status);
            sched_at = tick_ms() - 14u * 60 * 1000;     /* retry in a minute */
        }
        fetch_free(&sched_fetch);
    }
}

/* ---------- screens ---------- */

static void start_radio(void) {
    screen = SC_RADIO;
    report_stage("radio");
    pd->system->setAutoLockDisabled(1);
    if (sched && selected < 0) adopt_schedule(sched);  /* (not reached: adopt handles it) */
    if (sched) player_tune(selected >= 0 ? selected : 0);
    LOG("radio on");
}

static void begin_bench(void) {
    report_stage("bench:start");
    bench_start(&bench);
    screen = SC_BENCH;
    pd->system->getServerTime(on_server_time);
}

static int clock_left;
static void draw_header(void) {
    text(f_bold, "STATIC", 12, 6);
    struct PDDateTime dt;
    int64_t local = wall_ms() / 1000 - 946684800LL + pd->system->getTimezoneOffset();
    pd->system->convertEpochToDateTime((uint32_t)local, &dt);
    char clk[16];
    if (pd->system->shouldDisplay24HourTime()) snprintf(clk, sizeof clk, "%d:%02d", dt.hour, dt.minute);
    else snprintf(clk, sizeof clk, "%d:%02d %s", dt.hour % 12 ? dt.hour % 12 : 12, dt.minute, dt.hour < 12 ? "AM" : "PM");
    clock_left = 388 - text_w(f_bold, clk);
    text(f_bold, clk, clock_left, 6);
}

static void draw_intro(void) {
    pd->graphics->clear(kColorWhite);
    text_c(f_big, "STATIC", 200, 24);
    text_c(f_small, "Radio made of independent podcasts", 200, 56);
    pd->graphics->drawLine(40, 84, 360, 84, 1, kColorBlack);
    text_box(f_small, "This is a preview for testers. First a two-minute test checks how well a Playdate "
             "decodes and streams podcast audio, then the radio unlocks. Nothing plays aloud during the test.",
             28, 96, 344, 80);
    if (crash_note[0]) {
        char m[120];
        snprintf(m, sizeof m, "Last time Static stopped during \"%s\". We'll send that note too.", crash_note);
        text_box(f_small, m, 28, 172, 344, 40);
    }
    text_c(f_bold, bench_done_before ? "Ⓐ Start the radio    Ⓑ Run the test again" : "Ⓐ Start the test", 200, 216);
}

static void draw_net(void) {
    pd->graphics->clear(kColorWhite);
    text_c(f_big, "STATIC", 200, 40);
    const char *m = net_state == 1 ? "Turning on Wi-Fi…" : net_state == 2 ? "Asking for internet access…" : "Connected";
    text_c(f_small, m, 200, 110);
}

static void draw_nonet(void) {
    pd->graphics->clear(kColorWhite);
    text_c(f_big, "STATIC", 200, 30);
    const char *m = net_state == -2
        ? "Static needs internet access to stream. You can allow it under Settings > Permissions, then press Ⓐ to try again."
        : "Static needs Wi-Fi to stream. Check Settings > Wi-Fi, then press Ⓐ to try again.";
    text_box(f_small, m, 32, 90, 336, 80);
    if (net_msg[0]) text_c(f_small, net_msg, 200, 180);
}

static float clip_pct(int i) { return bench.clip[i].audio_s > 0 ? bench.clip[i].decode_s / bench.clip[i].audio_s * 100 : -1; }

static void draw_bench(void) {
    pd->graphics->clear(kColorWhite);
    draw_header();
    text(f_bold, "The two-minute test", 28, 34);
    for (int i = 0; i < BENCH_STEPS; i++) {
        int y = 62 + i * 26;
        char line[96];
        if (i < bench.step) pd->graphics->fillEllipse(30, y + 4, 10, 10, 0, 360, kColorBlack);
        else if (i == bench.step) pd->graphics->drawEllipse(30, y + 4, 10, 10, 2, 0, 360, kColorBlack);
        if (i < BENCH_CLIPS && i < bench.step) snprintf(line, sizeof line, "%s: %.0f%% of the CPU", bench_step_name(i), (double)clip_pct(i));
        else if (i == BENCH_CLIPS && i < bench.step) snprintf(line, sizeof line, "%s: %s", bench_step_name(i), bench.sched_ok ? "ok" : "failed");
        else if (i == BENCH_CLIPS + 1 && i <= bench.step) {
            int n = 0; float kb = 0;
            for (int k = 0; k < bench.nstreams; k++) if (bench.stream[k].bytes) { n++; kb += bench.stream[k].kbps; }
            if (n) snprintf(line, sizeof line, "%s: %d done, %.0f KB/s", bench_step_name(i), n, (double)(kb / n));
            else snprintf(line, sizeof line, "%s", bench_step_name(i));
        } else snprintf(line, sizeof line, "%s", bench_step_name(i));
        text(f_small, line, 48, y);
    }
    /* progress bar */
    int w = 344 * bench.step / BENCH_STEPS;
    pd->graphics->drawRect(28, 204, 344, 10, kColorBlack);
    pd->graphics->fillRect(28, 204, w, 10, kColorBlack);
}

static const char *verdict(float p) {
    if (p < 0) return "didn't run";
    if (p < 55) return "plenty of room";
    if (p < 80) return "fits";
    if (p < 95) return "tight";
    return "too slow";
}

static void draw_results(void) {
    pd->graphics->clear(kColorWhite);
    draw_header();
    text(f_bold, "Results", 28, 32);
    char line[120];
    static const char *names[BENCH_CLIPS] = { "MP3", "HE-AAC", "AAC-LC" };
    for (int i = 0; i < BENCH_CLIPS; i++) {
        float p = clip_pct(i);
        snprintf(line, sizeof line, "%s decoding: %.0f%% of the CPU, %s", names[i], (double)(p < 0 ? 0 : p), verdict(p));
        text(f_small, line, 28, 56 + i * 22);
    }
    int n = 0; float kb = 0; uint32_t tt = 0;
    for (int k = 0; k < bench.nstreams; k++) if (bench.stream[k].bytes > 100000) { n++; kb += bench.stream[k].kbps; tt += bench.stream[k].ttfb_ms; }
    if (n) snprintf(line, sizeof line, "Streaming: %.0f KB/s (a show needs about 16), first byte in %.1f s", (double)(kb / n), (double)tt / n / 1000.0);
    else snprintf(line, sizeof line, "Streaming: didn't get through (%s)", bench.nstreams ? bench.stream[0].err : "no schedule");
    text_box(f_small, line, 28, 122, 344, 40);
    int rs = report_status();
    if (rs == 1) text_c(f_small, "Sending…", 200, 172);
    else if (rs == 2) text_c(f_small, "Sent. Thank you!", 200, 172);
    else if (rs == 3) text_c(f_small, "Couldn't send. You can retry from the menu.", 200, 172);
    text_c(f_bold, "Ⓐ Send results and start the radio", 200, 196);
    text_c(f_small, "Ⓑ Start without sending", 200, 218);
}

static void noise_band(int y0, int y1, int density) {
    uint8_t *fb = pd->graphics->getFrame();
    if (!fb) return;
    for (int y = y0; y < y1; y++) {
        uint8_t *row = fb + y * LCD_ROWSIZE;
        for (int x = 0; x < 50; x++) {
            rng_ui ^= rng_ui << 13; rng_ui ^= rng_ui >> 17; rng_ui ^= rng_ui << 5;
            uint8_t m = (uint8_t)(rng_ui & (rng_ui >> 8));
            if (density < 2) m &= (uint8_t)(rng_ui >> 16);
            row[x] ^= m;
        }
    }
    pd->graphics->markUpdatedRows(y0, y1 - 1);
}

/* QR code for the episode page, cached per URL; returns its size in px (0 if
 * the link won't fit). Black modules on white with a quiet zone. */
static int draw_qr(const char *url, int x0, int y0, int max_px) {
    static uint8_t qr[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
    static char cached[1024];
    static int ok;
    if (strcmp(url, cached)) {
        static uint8_t tmp[qrcodegen_BUFFER_LEN_FOR_VERSION(15)];
        snprintf(cached, sizeof cached, "%s", url);
        ok = qrcodegen_encodeText(url, tmp, qr, qrcodegen_Ecc_LOW, 1, 15, qrcodegen_Mask_AUTO, true);
    }
    if (!ok) return 0;
    int n = qrcodegen_getSize(qr), quiet = 2;
    int scale = max_px / (n + 2 * quiet);
    if (scale < 1) return 0;
    int px = (n + 2 * quiet) * scale;
    pd->graphics->fillRect(x0, y0, px, px, kColorWhite);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++)
            if (qrcodegen_getModule(qr, x, y))
                pd->graphics->fillRect(x0 + (quiet + x) * scale, y0 + (quiet + y) * scale, scale, scale, kColorBlack);
    return px;
}

/* Episode info: a QR code to the episode's page, the show and the episode. */
static void draw_info(void) {
    const ep_t *ep = selected == player_station() ? player_episode() : NULL;
    if (!ep) { text_c(f_small, "Tuning…", 200, 110); text_c(f_small, "Ⓑ Stats", 200, 218); return; }
    char show[160], title[400];
    text_fold(ep->show, show, sizeof show);
    text_fold(ep->title, title, sizeof title);
    const char *link = ep->link && ep->link[0] ? ep->link : NULL;
    int qx = 8, qy = 28, qs = 0;
    if (link) qs = draw_qr(link, qx, qy, 186);
    int tx = qs ? qx + qs + 10 : 12, tw = 392 - tx;
    text_box(f_bold, show, tx, 34, tw, 60);
    text_box(f_small, title, tx, 96, tw, 80);
    text_box(f_small, qs ? "Scan for the episode" : link ? "Link too long for a QR code" : "No episode page for this one", tx, 176, tw, 40);
    text(f_small, "Ⓑ Stats", tx, 218);
}

static void draw_radio(void) {
    pd->graphics->clear(kColorWhite);
    draw_header();
    if (!sched) {
        text_c(f_big, "Tuning in", 200, 90);
        text_c(f_small, sched_fetching ? "Fetching today's schedule…" : "Waiting for the network…", 200, 130);
        noise_band(40, 80, 1);
        return;
    }
    if (view == 1) { draw_info(); return; }
    /* the dial */
    const int y0 = 32, y1 = 104, spacing = 150;
    pd->graphics->fillRect(0, y0, 400, y1 - y0, kColorBlack);
    pd->graphics->setDrawMode(kDrawModeFillWhite);
    for (int i = 0; i < sched->n; i++) {
        float x = 200 + ((float)i - dial) * spacing;
        if (x < -120 || x > 520) continue;
        text_c(f_big, sched->st[i].tag, (int)x, 52);
    }
    pd->graphics->setDrawMode(kDrawModeCopy);
    for (int k = -30; k <= 30; k++) {           /* ticks every tenth of a station */
        float pos = floorf(dial * 10) / 10 + k * 0.1f;
        int x = (int)(200 + (pos - dial) * spacing);
        if (x < 0 || x >= 400) continue;
        int major = fabsf(pos - roundf(pos)) < 0.01f;
        if (pos < -0.01f || pos > sched->n - 1 + 0.01f) continue;
        pd->graphics->drawLine(x, y1 - (major ? 12 : 5), x, y1 - 1, 1, kColorWhite);
    }
    pd->graphics->fillRect(199, y0, 3, y1 - y0, kColorWhite);   /* needle */
    pd->graphics->fillTriangle(193, y0 - 7, 207, y0 - 7, 200, y0, kColorBlack);
    pd->graphics->fillTriangle(193, y1 + 7, 207, y1 + 7, 200, y1, kColorBlack);
    int ps = player_state();
    int moving = fabsf(dial - roundf(dial)) > 0.02f || (selected != player_station());
    if (ps == PL_TUNING || ps == PL_TROUBLE || moving) noise_band(y0, y1 - 14, ps == PL_TUNING || moving ? 2 : 1);

    /* the phone's listener face + live count, under the needle: this station's */
    if (selected >= 0) {
        int c = report_count(sched->st[selected].id);
        if (selected == player_station() && player_state() == PL_PLAYING && c < 1) c = 1;   /* you're listening */
        if (c > 0) {
            char m[16]; snprintf(m, sizeof m, "%d", c);
            int x = 12;
            const uint32_t *face = k_faces[(int)((sched->epoch_ms / 86400000) * 7 % 11)];   /* daily_face.dart */
            for (int y = 0; y < 26; y++)
                for (int px = 0; px < 26; px++) {
                    if (!(face[y] >> (25 - px) & 1)) continue;
                    int run = 1;
                    while (px + run < 26 && (face[y] >> (25 - px - run) & 1)) run++;
                    pd->graphics->fillRect(x + px, 114 + y, run, 1, kColorBlack);
                    px += run - 1;
                }
            text(f_bold, m, x + 31, 118);
        }
    }
    if (view == 2) {
        const player_stats_t *s = player_stats();
        char l[120];
        int y = 144;
        float c0 = s->audio_s[0] > 0 ? s->decode_s[0] / s->audio_s[0] * 100 : -1;
        float c1 = s->audio_s[1] > 0 ? s->decode_s[1] / s->audio_s[1] * 100 : -1;
        float c2 = s->audio_s[2] > 0 ? s->decode_s[2] / s->audio_s[2] * 100 : -1;
        snprintf(l, sizeof l, "Decode CPU  MP3 %.0f%%  AAC %.0f%%  HE-AAC %.0f%%", (double)c0, (double)c1, (double)c2);
        text(f_small, l, 12, y); y += 18;
        snprintf(l, sizeof l, "Buffer %.1f s  drift %+lld ms  underruns %d", audio_buffered() / (double)RATE, (long long)player_drift_ms(), audio_underruns());
        text(f_small, l, 12, y); y += 18;
        snprintf(l, sizeof l, "Tunes %d, %.1f s avg to sound, %d failed", s->tunes, s->tune_ok ? s->tune_ms_sum / (double)s->tune_ok / 1000.0 : 0.0, s->tune_fail);
        text(f_small, l, 12, y); y += 18;
        snprintf(l, sizeof l, "Listened %u min  resyncs %d  %.0f fps", (unsigned)(s->listen_s / 60), s->resyncs, (double)app_fps);
        text(f_small, l, 12, y); y += 18;
        snprintf(l, sizeof l, "ID %s  v" APP_VERSION "   Ⓑ Back", report_device_id());
        text(f_small, l, 12, y);
        return;
    }
    const ep_t *ep = player_episode();
    if (ep && selected == player_station()) {
        char show[160], title[400];
        text_fold(ep->show, show, sizeof show);
        text_fold(ep->title, title, sizeof title);
        text_fit(f_bold, show, 12, 146, 376);
        text_box(f_small, title, 12, 168, 376, 40);
    }
    const char *st = player_status();
    char line[96];
    if (audio_muted()) snprintf(line, sizeof line, "Muted");
    else if (st[0]) snprintf(line, sizeof line, "%s", st);
    else line[0] = 0;
    if (line[0]) { char f[96]; snprintf(f, sizeof f, "%s", line); text(f_small, f, 388 - text_w(f_small, f), 118); }
    text_c(f_small, (pd->system->isCrankDocked() && !autopilot) ? "⬅️ Tune (or undock the crank)   Ⓐ Mute   Ⓑ Info" : "Crank to tune    Ⓐ Mute    Ⓑ Info", 200, 218);
}

/* ---------- input ---------- */

static void radio_input(void) {
    if (!sched) return;
    PDButtons cur, pushed, rel;
    pd->system->getButtonState(&cur, &pushed, &rel);
    float change = pd->system->getCrankChange();
    if (ap_video) { change += ap_crank_change; ap_crank_change = 0; }
    int n = sched->n;
    if (change != 0) {
        dial += change / 120.0f;               /* a third of a turn per station */
        dial_target = -1;
        crank_idle_at = tick_ms();
    }
    if (pushed & (kButtonRight | kButtonLeft)) {
        int base = (int)roundf(dial_target >= 0 ? dial_target : dial);
        base += (pushed & kButtonRight) ? 1 : -1;
        if (base < 0) base = 0;
        if (base > n - 1) base = n - 1;
        dial_target = (float)base;
    }
    if (dial < -0.4f) dial = -0.4f;
    if (dial > n - 1 + 0.4f) dial = n - 1 + 0.4f;
    if (dial_target >= 0) {                    /* d-pad: glide */
        dial += (dial_target - dial) * 0.35f;
        if (fabsf(dial_target - dial) < 0.01f) { dial = dial_target; dial_target = -1; }
    } else if (tick_ms() - crank_idle_at > 180) {   /* crank at rest: settle into the detent */
        float r = roundf(dial);
        if (r < 0) r = 0;
        if (r > n - 1) r = (float)(n - 1);
        dial += (r - dial) * 0.3f;
        if (fabsf(r - dial) < 0.005f) dial = r;
    }
    int near = (int)roundf(dial);
    if (near < 0) near = 0;
    if (near > n - 1) near = n - 1;
    if (near != selected) { selected = near; selected_at = tick_ms(); }
    if (selected != player_station() && tick_ms() - selected_at > 300 && fabsf(dial - selected) < 0.15f) {
        player_tune(selected);
        data_write("last.txt", sched->st[selected].id, (int)strlen(sched->st[selected].id));
        char stage[48]; snprintf(stage, sizeof stage, "radio:%s", sched->st[selected].id);
        report_stage(stage);
    }
    if (pushed & kButtonA) audio_mute(!audio_muted());
    if (pushed & kButtonB) view = (view + 1) % 3;
}

/* ---------- menu ---------- */

static void menu_report(void *ud) {
    (void)ud;
    report_send_session();
    if (bench.done) report_send_bench(&bench);
}
static void menu_rerun(void *ud) {
    (void)ud;
    player_tune(-1);
    begin_bench();
}

/* ---------- main loop ---------- */

static void vid_log_episode(void) {
    const ep_t *ep = player_episode();
    int slot; int64_t off = 0;
    if (sched && player_station() >= 0) sched_live(sched, player_station(), wall_ms(), &slot, &off);
    LOG("video: t=%u ms on %s: %s | %s | at %lld s | %s", (unsigned)(tick_ms() - vid_t0),
        sched && player_station() >= 0 ? sched->st[player_station()].id : "?",
        ep ? ep->show : "", ep ? ep->title : "", (long long)(off / 1000), ep ? ep->url : "");
}

static void video_step(void) {
    if (!sched) return;
    int playing_here = player_state() == PL_PLAYING && selected == player_station();
    switch (vid_phase) {
    case 0:   /* wait for the first station's sound, then roll */
        if (playing_here && tick_ms() - player_playing_since() > 600) {
            vid_f = pd->file->open("vid_frames.bin", kFileWrite);
            audio_tap_begin(90);
            vid_t0 = vid_t = tick_ms();
            vid_phase = 1; vid_i = 0;
            LOG("video: rolling");
            vid_log_episode();
        }
        break;
    case 1:   /* hold on this station */
        if (tick_ms() - vid_t > 3500) {
            if (++vid_i < vid_n) { vid_phase = 2; vid_t = tick_ms(); vid_last_tick = tick_ms(); }
            else { vid_phase = 4; vid_t = tick_ms(); view = 1; }
        }
        break;
    case 2: { /* turn the crank toward the next station */
        int target = station_index(vid_ids[vid_i]);
        if (target < 0) { vid_phase = 4; vid_t = tick_ms(); view = 1; break; }
        float dt = (tick_ms() - vid_last_tick) / 1000.0f;
        vid_last_tick = tick_ms();
        float dir = target > dial ? 1.0f : -1.0f;
        if (fabsf(target - dial) > 0.12f) ap_crank_change += dir * 190.0f * dt;   /* ~1.6 stations a second */
        else { vid_phase = 3; vid_t = tick_ms(); }
        break;
    }
    case 3:   /* settled: wait for the sound, then hold */
        if (playing_here) { vid_phase = 1; vid_t = tick_ms(); vid_log_episode(); }
        else if (tick_ms() - vid_t > 15000) { LOG("video: no sound on %s", vid_ids[vid_i]); vid_phase = 1; vid_t = tick_ms(); }
        break;
    case 4:   /* the episode info, then cut */
        if (tick_ms() - vid_t > 2800) {
            if (vid_f) { pd->file->close(vid_f); vid_f = NULL; }
            int n = 0;
            int16_t *a = audio_tap_end(&n);
            if (a) { data_write("vid_audio.raw", a, n * 2); free(a); }
            char meta[64]; int ml = snprintf(meta, sizeof meta, "%u\n", (unsigned)vid_t0);
            data_write("vid_meta.txt", meta, ml);
            LOG("video: cut, %d audio samples", n);
            view = 0;
            vid_phase = 5;
            ap_done = 1; report_stage("clean"); data_write("autotest_done", "1", 1);
        }
        break;
    }
}

static void video_capture(void) {
    if (!vid_f || vid_phase < 1 || vid_phase > 4) return;
    uint8_t *fb = pd->graphics->getFrame();
    if (!fb) return;
    uint32_t t = tick_ms();
    pd->file->write(vid_f, &t, 4);
    pd->file->write(vid_f, fb, LCD_ROWSIZE * LCD_ROWS);
}

static uint32_t perf_at, perf_frames;
static int64_t perf_bytes;
float app_fps = 0;

static int update(void *ud) {
    (void)ud;
    pd->system->resetElapsedTime();
    perf_frames++;
    if (tick_ms() - perf_at >= 10000) {
        uint32_t ms = tick_ms() - perf_at;
        int64_t b = net_bytes_total();
        if (perf_at) app_fps = perf_frames * 1000.0f / ms;
        if (perf_at) LOG("perf: %.1f fps, network %.0f KB/s, buffer %d ms, player %d",
                         perf_frames * 1000.0 / ms, (b - perf_bytes) / 1.024 / ms, audio_buffered() * 1000 / RATE, player_state());
        perf_at = tick_ms(); perf_frames = 0; perf_bytes = b;
    }
    net_tick();
    report_update();

    PDButtons cur, pushed, rel;
    if (autopilot && tick_ms() - ap_shot_at > 6000) { ap_save_screen(); ap_shot_at = tick_ms(); }
    switch (screen) {
    case SC_INTRO:
        pd->system->getButtonState(&cur, &pushed, &rel);
        if (autopilot && tick_ms() - app_start > 2500 && !ap_since) { ap_since = 1; net_bring_up(ap_radio_only ? SC_RADIO : SC_BENCH); }
        else if (pushed & kButtonA) net_bring_up(bench_done_before ? SC_RADIO : SC_BENCH);
        else if ((pushed & kButtonB) && bench_done_before) net_bring_up(SC_BENCH);
        draw_intro();
        break;
    case SC_NET:
        if (net_state == 3) {
            if (crash_note[0]) { report_send_crash(crash_note); crash_note[0] = 0; }
            if (after_net == SC_BENCH) begin_bench();
            else { pd->system->getServerTime(on_server_time); start_radio(); }
        } else if (net_state < 0) screen = SC_NONET;
        draw_net();
        break;
    case SC_NONET:
        pd->system->getButtonState(&cur, &pushed, &rel);
        if (pushed & kButtonA) net_bring_up(after_net);
        draw_nonet();
        break;
    case SC_BENCH: {
        sched_t *got = NULL;
        bench_update(&bench, 0.025f, &got);
        if (got) adopt_schedule(got);
        if (bench.step < BENCH_STEPS) {
            char stage[48]; snprintf(stage, sizeof stage, "bench:%s", bench_step_name(bench.step));
            static int last_step = -1;
            if (bench.step != last_step) { report_stage(stage); last_step = bench.step; }
        }
        if (bench.done) {
            data_write("bench_done", "1", 1);
            bench_done_before = 1;
            screen = SC_RESULTS;
            report_choice_made = 0;
            report_stage("results");
            player_tune(-1);
            ap_since = tick_ms();
        }
        draw_bench();
        break;
    }
    case SC_RESULTS:
        pd->system->getButtonState(&cur, &pushed, &rel);
        if (autopilot && !report_choice_made && tick_ms() - ap_since > 4000) pushed |= kButtonA;
        if (!report_choice_made && (pushed & kButtonA)) { report_send_bench(&bench); report_choice_made = 1; start_radio(); }
        else if (!report_choice_made && (pushed & kButtonB)) { report_choice_made = 1; start_radio(); }
        draw_results();
        break;
    case SC_RADIO: {
        schedule_tick();
        if (autopilot && !ap_done) {
            if (!ap_since || ap_since < 10) ap_since = tick_ms();
            if (ap_video) video_step();
            else if (ap_radio_only) view = (tick_ms() - ap_since > 22000 && tick_ms() - ap_since < 34000) ? 1 : 0;
            if (!ap_video && sched && tick_ms() - ap_since > 40000) {
                ap_since = tick_ms();
                if (ap_radio_only) { ap_done = 1; report_stage("clean"); data_write("autotest_done", "1", 1); LOG("autopilot: done"); }
                else if (ap_steps++ < sched->n) dial_target = (float)((selected + 1) % sched->n);
                else { ap_done = 1; report_send_session(); data_write("autotest_done", "1", 1); LOG("autopilot: done"); }
            }
        }
        radio_input();
        if (sched && player_station() >= 0 && player_station() < sched->n && player_state() == PL_PLAYING) report_set_presence(sched->st[player_station()].id);
        else if (player_state() == PL_OFF) report_set_presence("");
        /* decode for most of the frame (30 fps = 33 ms), leave room to draw */
        player_update(0.022f);
        uint32_t now = tick_ms();
        int animating = fabsf(dial - roundf(dial)) > 0.002f || dial_target >= 0 || player_state() == PL_TUNING || selected != player_station();
        if (animating || now - last_draw > 500 || ap_video) { draw_radio(); last_draw = now; }
        if (ap_video) video_capture();
        break;
    }
    }
    return 1;
}

#ifdef _WINDLL
__declspec(dllexport)
#endif
int eventHandler(PlaydateAPI *playdate, PDSystemEvent event, uint32_t arg) {
    (void)arg;
    switch (event) {
    case kEventInit: {
        pd = playdate;
        app_start = pd->system->getCurrentTimeMilliseconds();
        /* a small rolling log in Data/ for anyone who wants to send it */
        FileStat st;
        if (pd->file->stat("log.txt", &st) == 0 && st.size > 256 * 1024) pd->file->unlink("log.txt", 0);
        log_file = pd->file->open("log.txt", kFileAppend);
        LOG("Static for Playdate " APP_VERSION " starting");
        const char *err = NULL;
        f_big = pd->graphics->loadFont("fonts/Roobert-24-Medium", &err);
        f_bold = pd->graphics->loadFont("fonts/Roobert-11-Bold", &err);
        f_small = pd->graphics->loadFont("fonts/Asheville-Sans-14-Light", &err);
        if (!f_big || !f_bold || !f_small) LOG("font load: %s", err ? err : "?");
        if (!f_small) f_small = f_bold;
        net_init();
        audio_init();
        player_init();
        report_init();
        const char *crash = report_last_crash();
        if (crash) { snprintf(crash_note, sizeof crash_note, "%s", crash); LOG("previous run stopped during: %s", crash); }
        report_stage("intro");
        int len = 0;
        char *ap = data_read("autotest.txt", &len);
        autopilot = ap != NULL;
        if (ap && !strncmp(ap, "video:", 6)) {
            ap_video = ap_radio_only = 1;
            char *nl = strpbrk(ap, "\r\n"); if (nl) *nl = 0;
            for (char *tok = strtok(ap + 6, ","); tok && vid_n < 8; tok = strtok(NULL, ","))
                snprintf(vid_ids[vid_n++], sizeof vid_ids[0], "%s", tok);
            if (vid_n) data_write("last.txt", vid_ids[0], (int)strlen(vid_ids[0]));
        } else if (ap && !strncmp(ap, "radio:", 6)) {
            ap_radio_only = 1;
            char *nl = strpbrk(ap, "\r\n"); if (nl) *nl = 0;
            data_write("last.txt", ap + 6, (int)strlen(ap + 6));
        }
        free(ap);
        if (autopilot) { LOG("autopilot on"); audio_test_silent(1); }
        LOG("time zone offset %d s, 24-hour %d", (int)pd->system->getTimezoneOffset(), pd->system->shouldDisplay24HourTime());
        char *done = data_read("bench_done", &len);
        bench_done_before = done != NULL;
        free(done);
        mi_report = pd->system->addMenuItem("send report", menu_report, NULL);
        mi_rerun = pd->system->addMenuItem("rerun test", menu_rerun, NULL);
        pd->display->setRefreshRate(30);
        pd->system->setUpdateCallback(update, NULL);
        break;
    }
    case kEventTerminate:
        report_clean_exit();
        LOG("exit");
        if (log_file) { pd->file->close(log_file); log_file = NULL; }
        break;
    case kEventLock:
        LOG("locked");
        break;
    case kEventUnlock:
        LOG("unlocked");
        break;
    default:
        break;
    }
    return 0;
}
