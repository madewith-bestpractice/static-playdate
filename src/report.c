#include "common.h"
#include "report.h"
#include "audio.h"
#include "json.h"
#include "net.h"
#include "player.h"

/* Presence: the phone's contract (presence_manager.dart). PUT
 * {rtdb}/presence/{station}/{device}.json {"ts":{".sv":"timestamp"}} every
 * 30 s (live for 75 s); DELETE when leaving. Counts: GET /api/counts. */
#define RTDB "https://static-3c8b6-default-rtdb.firebaseio.com"
#define COUNTS_URL "https://staticradio.app/api/counts"
#define HEARTBEAT_MS 30000
#define COUNTS_MS 60000
#define QN 8

typedef struct { char method[8]; char url[160]; char *body; int is_report; } req_t;
static req_t q[QN];
static int nq;
static fetch_t cur;
static int cur_active, cur_is_report, cur_is_counts;
static int rstatus;
static char device_id[20];
static char crash_stage[64];
static int have_crash;
static char presence_want[24], presence_on[24];
static uint32_t presence_at, counts_at;
static struct { char id[24]; int n; } counts[16];
static int ncounts;

const char *report_device_id(void) { return device_id; }

void report_init(void) {
    int len = 0;
    char *id = data_read("id.txt", &len);
    if (id && len >= 8 && len < (int)sizeof device_id) snprintf(device_id, sizeof device_id, "%s", id);
    else {
        unsigned ms = 0;
        unsigned s = pd->system->getSecondsSinceEpoch(&ms);
        uint32_t x = s * 2654435761u ^ ms * 40503u ^ tick_ms() * 2246822519u ^ (uint32_t)(pd->system->getCrankAngle() * 1000) ^ 0x9e3779b9u;
        char hex[17];
        for (int i = 0; i < 16; i++) {
            x ^= x << 13; x ^= x >> 17; x ^= x << 5;
            hex[i] = "0123456789abcdef"[x & 15];
        }
        hex[16] = 0;
        snprintf(device_id, sizeof device_id, "pd-%s", hex);
        data_write("id.txt", device_id, (int)strlen(device_id));
    }
    free(id);
    char *st = data_read("stage.txt", &len);
    if (st && len > 0 && strcmp(st, "clean")) { snprintf(crash_stage, sizeof crash_stage, "%s", st); have_crash = 1; }
    free(st);
}

void report_stage(const char *stage) { data_write("stage.txt", stage, (int)strlen(stage)); }
void report_clean_exit(void) { report_stage("clean"); }
const char *report_last_crash(void) { return have_crash ? crash_stage : NULL; }
int report_status(void) { return rstatus; }

static int enqueue(const char *method, const char *url, char *body, int is_report) {
    if (nq >= QN) { free(body); return 0; }
    req_t *r = &q[nq++];
    snprintf(r->method, sizeof r->method, "%s", method);
    snprintf(r->url, sizeof r->url, "%s", url);
    r->body = body;
    r->is_report = is_report;
    if (is_report) rstatus = 1;
    return 1;
}

static char *event_body(const char *name, const char *params) {
    int64_t ts = wall_ms();
    const struct PDInfo *info = pd->system->getSystemInfo();
    int n = 600 + (int)strlen(params);
    char *b = malloc((size_t)n);
    if (!b) return NULL;
    snprintf(b, (size_t)n,
        "{\"surface\":\"playdate\",\"deviceId\":\"%s\",\"events\":[{\"name\":\"%s\",\"ts\":%lld,\"params\":{"
        "\"app\":\"" APP_VERSION "\",\"os\":%u,\"sim\":%d,\"batt\":%.0f,%s}}]}",
        device_id, name, (long long)ts, info ? (unsigned)info->osversion : 0,
#if TARGET_SIMULATOR
        1,
#else
        0,
#endif
        (double)pd->system->getBatteryPercentage(), params);
    return b;
}

static float pct(float cpu, float audio) { return audio > 0 ? cpu / audio * 100.0f : -1.0f; }

int report_send_bench(const bench_t *b) {
    char p[1700];
    int o = snprintf(p, sizeof p,
        "\"mp3_cpu\":%.1f,\"heaac_cpu\":%.1f,\"lc_cpu\":%.1f,\"dec_err\":[%d,%d,%d],"
        "\"sched_ok\":%d,\"sched_ms\":%u,\"sched_kb\":%d,\"clock_off_ms\":%lld,\"clock_known\":%d,\"streams\":[",
        (double)pct(b->clip[0].decode_s, b->clip[0].audio_s), (double)pct(b->clip[1].decode_s, b->clip[1].audio_s),
        (double)pct(b->clip[2].decode_s, b->clip[2].audio_s), b->clip[0].errors, b->clip[1].errors, b->clip[2].errors,
        b->sched_ok, (unsigned)b->sched_ms, b->sched_kb, (long long)b->clock_off_ms, b->clock_known);
    for (int i = 0; i < b->nstreams && o < (int)sizeof p - 200; i++) {
        const bench_stream_t *s = &b->stream[i];
        o += snprintf(p + o, sizeof p - (size_t)o, "%s{\"st\":\"%s\",\"http\":%d,\"hops\":%d,\"ttfb\":%u,\"kbs\":%.1f,\"kb\":%d,\"err\":\"%s\"}",
                      i ? "," : "", s->station, s->status, s->hops, (unsigned)s->ttfb_ms, (double)s->kbps, s->bytes / 1024, s->err);
    }
    snprintf(p + o, sizeof p - (size_t)o, "]");
    char *body = event_body("pd_bench", p);
    return body ? enqueue("POST", EVENT_URL, body, 1) : 0;
}

int report_send_session(void) {
    const player_stats_t *s = player_stats();
    char p[1200];
    snprintf(p, sizeof p,
        "\"listen_s\":%u,\"tunes\":%d,\"tune_ok\":%d,\"tune_fail\":%d,\"tune_ms_avg\":%u,\"tune_ms_max\":%u,"
        "\"underruns\":%d,\"stream_fail\":%d,\"reconnects\":%d,\"resyncs\":%d,\"unsupported\":%d,"
        "\"cpu\":[%.1f,%.1f,%.1f,%.1f],\"audio_s\":[%.0f,%.0f,%.0f,%.0f],\"moov_kb\":%d,\"moov_ms\":%u,\"drift_ms\":%lld,\"fps\":%.1f",
        (unsigned)s->listen_s, s->tunes, s->tune_ok, s->tune_fail, (unsigned)(s->tune_ok ? s->tune_ms_sum / (uint32_t)s->tune_ok : 0), (unsigned)s->tune_ms_max,
        audio_underruns(), s->stream_fail, s->reconnects, s->resyncs, s->unsupported,
        (double)pct(s->decode_s[0], s->audio_s[0]), (double)pct(s->decode_s[1], s->audio_s[1]),
        (double)pct(s->decode_s[2], s->audio_s[2]), (double)pct(s->decode_s[3], s->audio_s[3]),
        (double)s->audio_s[0], (double)s->audio_s[1], (double)s->audio_s[2], (double)s->audio_s[3],
        s->moov_kb_max, (unsigned)s->moov_ms_max, (long long)player_drift_ms(), (double)app_fps);
    char *body = event_body("pd_session", p);
    return body ? enqueue("POST", EVENT_URL, body, 1) : 0;
}

int report_send_crash(const char *stage) {
    char p[160];
    char clean[64]; int k = 0;
    for (const char *c = stage; *c && k < 63; c++) if (*c != '"' && *c != '\\' && (unsigned char)*c >= 32) clean[k++] = *c;
    clean[k] = 0;
    snprintf(p, sizeof p, "\"stage\":\"%s\"", clean);
    have_crash = 0;
    char *body = event_body("pd_crash", p);
    return body ? enqueue("POST", EVENT_URL, body, 1) : 0;
}

void report_set_presence(const char *id) { snprintf(presence_want, sizeof presence_want, "%s", id ? id : ""); }

int report_count(const char *id) {
    for (int i = 0; i < ncounts; i++) if (!strcmp(counts[i].id, id)) return counts[i].n;
    return -1;
}

static void parse_counts(char *text) {
    jpool *pool = NULL;
    jnode *root = json_parse(text, &pool);
    const jnode *c = root ? json_get(root, "counts") : NULL;
    if (c && c->t == J_OBJ) {
        ncounts = 0;
        for (const jnode *e = c->child; e && ncounts < 16; e = e->next) {
            snprintf(counts[ncounts].id, sizeof counts[ncounts].id, "%s", e->key);
            counts[ncounts].n = e->t == J_NUM ? (int)e->n : 0;
            ncounts++;
        }
    }
    json_free(pool);
}

void report_update(void) {
    /* presence: leave the old station, heartbeat the current one */
    if (strcmp(presence_want, presence_on)) {
        char url[160];
        if (presence_on[0]) {
            snprintf(url, sizeof url, RTDB "/presence/%s/%s.json", presence_on, device_id);
            enqueue("DELETE", url, NULL, 0);
        }
        snprintf(presence_on, sizeof presence_on, "%s", presence_want);
        presence_at = 0;
    }
    if (presence_on[0] && (!presence_at || tick_ms() - presence_at > HEARTBEAT_MS) && nq < QN - 2) {
        char url[160];
        snprintf(url, sizeof url, RTDB "/presence/%s/%s.json", presence_on, device_id);
        enqueue("PUT", url, strdup("{\"ts\":{\".sv\":\"timestamp\"}}"), 0);
        presence_at = tick_ms();
    }
    if (presence_on[0] && (!counts_at || tick_ms() - counts_at > COUNTS_MS) && nq < QN - 2) {
        enqueue("GET", COUNTS_URL, NULL, 0);
        counts_at = tick_ms();
    }

    if (!cur_active && nq > 0) {
        req_t r = q[0];
        memmove(q, q + 1, sizeof q[0] * (size_t)(nq - 1));
        nq--;
        cur_is_report = r.is_report;
        cur_is_counts = !strcmp(r.url, COUNTS_URL);
        fetch_start(&cur, r.url, r.method, r.body, r.body ? (int)strlen(r.body) : 0, "application/json", 64 * 1024);
        free(r.body);
        cur_active = 1;
    }
    if (cur_active && fetch_poll(&cur)) {
        int ok = cur.done == 1 && cur.status >= 200 && cur.status < 300;
        if (cur_is_report) { rstatus = ok ? 2 : 3; LOG("report: %s (HTTP %d) %.80s", ok ? "sent" : "FAILED", cur.status, cur.buf ? cur.buf : ""); }
        if (cur_is_counts && ok && cur.buf) parse_counts(cur.buf);
        fetch_free(&cur);
        cur_active = 0;
    }
}
