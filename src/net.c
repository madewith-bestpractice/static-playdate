#include "common.h"
#include "net.h"

#define MAX_HOPS 10
#define GRAVE_N 64
#define RELEASE_AFTER_MS 1500
#define CONNECT_TIMEOUT_MS 15000
#define LOW_CONNECT_TIMEOUT_MS 8000   /* background warming gives up sooner */
#define STALL_MS 12000
#define READ_TIMEOUT_MS 3000          /* we only read what's available; never let read() wait long */
#define WAIT_N 24
#define QUEUE_WAIT_MS 30000           /* a request that can't start in this long fails */
#define FLIGHT_N 8
#define OPEN_LOST_MS 45000            /* an open with no answer in this long stops holding the line */
#define SLOW_CALL_MS 200

static const struct playdate_tcp *tcp;
/* tcp->open queues on one network thread and each connect blocks it, so an open
 * can run many seconds after we gave up on its socket (a device takes 7-20 s to
 * the first byte). Releasing a connection that an open or close still has to
 * reach crashes inside the system, on a queued open or in on_open. So a socket
 * we're done with waits here: for its own open to call back, then it's closed,
 * and it's released only after a rest with no open in flight anywhere.
 *
 * And only one open goes to the system at a time. On a device, opens piled up
 * behind one that never answered (a reconnect every 18 s, then a tune every
 * 2 s), and the next network call stalled the run loop past the 10 s watchdog.
 * So requests wait in our own queue (waitq), best priority first, where a
 * retune can drop them for free; closes wait until no open is in flight. */
enum { G_OPENING, G_CLOSE, G_CLOSED };
static struct { TCPConnection *c; uint32_t at; int st; } grave[GRAVE_N];
static int ngrave, nopen;
static int64_t bytes_total;

/* opens the system is working on: one we wait for, plus any that never answered */
static struct { TCPConnection *c; uint32_t at; int lost; char host[48]; } fl[FLIGHT_N];
static int nfl, nlost_total;

struct hs {
    char url[4096];
    char method[8];
    int64_t from, to;
    char *body; int blen;
    char ctype[48];
    TCPConnection *c;
    int st, status, hops, chunked;
    int64_t clen, total, range_start, got;
    char hdr[8192]; int hdr_len;
    uint8_t pend[8192]; int pend_len, pend_off;   /* raw body bytes not yet handed out */
    int64_t chunk_left; int chunk_phase; char cl[24]; int cl_len;
    int opened, open_err, closed;
    char req[4700]; int req_len, req_off;
    uint32_t t_start, t_hop, t_first, t_last, t_queued;
    char host[256]; int port, ssl;
    int prio, waiting, connect_ms;
    char err[64];
};

static hs_t *waitq[WAIT_N];
static int nwait;

/* Time the system calls: a log line names any that blocked the run loop. */
static uint32_t call_t0;
static void slow(const char *what) {
    static uint32_t last; static int quiet;
    uint32_t dt = tick_ms() - call_t0;
    if (dt < SLOW_CALL_MS) return;
    if (tick_ms() - last < 1000) { quiet++; return; }
    LOG("net: %s took %u ms (blocked the frame)%s", what, (unsigned)dt, quiet ? ", and others" : "");
    last = tick_ms(); quiet = 0;
}
#define TIMED(what, call) do { call_t0 = tick_ms(); call; slow(what); } while (0)

static int active_opens(void) {
    int n = 0;
    for (int i = 0; i < nfl; i++) n += !fl[i].lost;
    return n;
}

static int flight_find(TCPConnection *c) {
    for (int i = 0; i < nfl; i++) if (fl[i].c == c) return i;
    return -1;
}

static void unqueue(hs_t *h) {
    if (!h->waiting) return;
    int k = 0;
    for (int i = 0; i < nwait; i++) if (waitq[i] != h) waitq[k++] = waitq[i];
    nwait = k;
    h->waiting = 0;
}

/* Simulator test hooks (autotest.txt): pretend connects are slow, or that one
 * never answers, as on a device. */
static int dbg_delay_ms, dbg_hang_nth, dbg_submits;
static struct { TCPConnection *c; PDNetErr err; uint32_t due; } dbg_q[FLIGHT_N];
static int dbg_nq;
static TCPConnection *dbg_hung;
static int64_t dbg_cut_bytes; static int dbg_outage_ms, dbg_cut_done;
static uint32_t dbg_outage_until;
void net_debug(int open_delay_ms, int hang_nth) {
    dbg_delay_ms = open_delay_ms; dbg_hang_nth = hang_nth;
    LOG("net: test mode, opens answer after %d ms%s", open_delay_ms, hang_nth ? ", one never answers" : "");
}
void net_debug_outage(int64_t cut_after_bytes, int outage_ms) {
    dbg_cut_bytes = cut_after_bytes; dbg_outage_ms = outage_ms;
    LOG("net: test mode, the stream drops after %lld KB, then every open fails for %d s", (long long)(cut_after_bytes / 1024), outage_ms / 1000);
}

void net_init(void) { tcp = pd->network->tcp; }
int net_open_count(void) { return nopen; }
int64_t net_bytes_total(void) { return bytes_total; }

static void bury(TCPConnection *c, int open_pending) {
    if (!c) return;
    TIMED("setUserdata", tcp->setUserdata(c, NULL));
    nopen--;
    if (ngrave == GRAVE_N) {            /* never released: a small leak beats a crash */
        static int said;
        if (!said) { said = 1; LOG("net: socket grave full (%d opens in flight), leaking sockets from here", nfl); }
        if (!open_pending) TIMED("close", tcp->close(c));
        return;
    }
    grave[ngrave].c = c;
    grave[ngrave].at = tick_ms();
    grave[ngrave].st = open_pending ? G_OPENING : G_CLOSE;
    ngrave++;
}

static void fail(hs_t *h, const char *why);
static void deliver_open(TCPConnection *c, PDNetErr err);
static void on_open(TCPConnection *c, PDNetErr err, void *ud);
static void on_closed(TCPConnection *c, PDNetErr err);

/* hand one queued request to the system */
static void submit(hs_t *h) {
    unqueue(h);
    h->t_hop = tick_ms();
    h->connect_ms = h->prio == NET_PRIO_LOW ? LOW_CONNECT_TIMEOUT_MS : CONNECT_TIMEOUT_MS;
    TIMED("newConnection", h->c = tcp->newConnection(h->host, h->port, h->ssl));
    if (!h->c) { fail(h, "no socket"); return; }
    nopen++;
    tcp->setUserdata(h->c, h);
    tcp->setConnectTimeout(h->c, h->connect_ms);
    tcp->setReadTimeout(h->c, READ_TIMEOUT_MS);
    tcp->setReadBufferSize(h->c, 32 * 1024);
    tcp->setConnectionClosedCallback(h->c, on_closed);
    fl[nfl].c = h->c; fl[nfl].at = tick_ms(); fl[nfl].lost = 0;
    snprintf(fl[nfl].host, sizeof fl[nfl].host, "%.47s", h->host);
    nfl++;
    dbg_submits++;
    if (dbg_hang_nth && dbg_submits == dbg_hang_nth) dbg_hung = h->c;
    PDNetErr e;
    TIMED("open", e = tcp->open(h->c, on_open, NULL));
    if (e != NET_OK) {                   /* refused: no callback will come */
        char m[32]; snprintf(m, sizeof m, "open err %d", e);
        int k = flight_find(h->c);
        if (k >= 0) fl[k] = fl[--nfl];
        h->opened = 1;
        fail(h, m);
    }
}

void net_tick(void) {
    uint32_t now = tick_ms();
    for (int i = 0; i < dbg_nq; ) {
        if ((int32_t)(now - dbg_q[i].due) >= 0) {
            TCPConnection *c = dbg_q[i].c; PDNetErr e = dbg_q[i].err;
            dbg_q[i] = dbg_q[--dbg_nq];
            deliver_open(c, e);
        } else i++;
    }
    for (int i = 0; i < nfl; i++)
        if (!fl[i].lost && now - fl[i].at > OPEN_LOST_MS) {
            fl[i].lost = 1; nlost_total++;
            LOG("net: no answer from %s in %u s; not waiting on it any more", fl[i].host, (unsigned)(now - fl[i].at) / 1000);
        }
    int active = active_opens();
    int k = 0;
    for (int i = 0; i < ngrave; i++) {
        if (grave[i].st == G_CLOSE && !active) { TIMED("close", tcp->close(grave[i].c)); grave[i].st = G_CLOSED; grave[i].at = tick_ms(); }
        if (grave[i].st == G_CLOSED && !nfl && tick_ms() - grave[i].at > RELEASE_AFTER_MS) TIMED("release", tcp->release(grave[i].c));
        else grave[k++] = grave[i];
    }
    ngrave = k;
    /* one open at a time: the best waiting request, oldest first */
    if (!active && nwait && nfl < FLIGHT_N) {
        int best = 0;
        for (int i = 1; i < nwait; i++) if (waitq[i]->prio > waitq[best]->prio) best = i;
        submit(waitq[best]);
    }
}

int net_stuck(void) { return nfl - active_opens(); }

void net_describe(char *buf, int cap) {
    snprintf(buf, (size_t)cap, "sockets %d, connecting %d, stuck %d (%d so far), queued %d, closing %d",
             nopen, active_opens(), net_stuck(), nlost_total, nwait, ngrave);
}

static int parse_url(const char *url, char *host, size_t hcap, int *port, int *ssl, const char **path) {
    const char *p = url;
    if (!strncmp(p, "https://", 8)) { *ssl = 1; *port = 443; p += 8; }
    else if (!strncmp(p, "http://", 7)) { *ssl = 0; *port = 80; p += 7; }
    else return -1;
    const char *slash = strchr(p, '/');
    const char *q = strchr(p, '?');
    if (q && (!slash || q < slash)) slash = q;
    size_t hl = slash ? (size_t)(slash - p) : strlen(p);
    if (hl == 0 || hl >= hcap) return -1;
    memcpy(host, p, hl); host[hl] = 0;
    char *colon = strchr(host, ':');
    if (colon) { *port = atoi(colon + 1); *colon = 0; }
    *path = slash ? slash : "/";
    return 0;
}

static void resolve(const char *base, const char *loc, char *out, size_t cap) {
    if (!strncmp(loc, "http://", 7) || !strncmp(loc, "https://", 8)) { snprintf(out, cap, "%s", loc); return; }
    const char *se = strstr(base, "://");
    if (!se) { snprintf(out, cap, "%s", loc); return; }
    if (!strncmp(loc, "//", 2)) { snprintf(out, cap, "%.*s:%s", (int)(se - base), base, loc); return; }
    const char *he = strchr(se + 3, '/');
    int hl = he ? (int)(he - base) : (int)strlen(base);
    if (loc[0] == '/') { snprintf(out, cap, "%.*s%s", hl, base, loc); return; }
    const char *q = strchr(base, '?');
    const char *last = NULL;
    for (const char *p = se + 3; *p && (!q || p < q); p++) if (*p == '/') last = p;
    if (last) snprintf(out, cap, "%.*s%s", (int)(last - base) + 1, base, loc);
    else snprintf(out, cap, "%.*s/%s", hl, base, loc);
}

int hs_header(const hs_t *h, const char *name, char *v, int cap) {
    size_t nl = strlen(name);
    for (const char *p = h->hdr; (p = strstr(p, "\r\n")); ) {
        p += 2;
        if (!strncasecmp(p, name, nl) && p[nl] == ':') {
            const char *s = p + nl + 1;
            while (*s == ' ' || *s == '\t') s++;
            const char *e = strstr(s, "\r\n");
            int l = e ? (int)(e - s) : (int)strlen(s);
            if (l >= cap) l = cap - 1;
            memcpy(v, s, (size_t)l); v[l] = 0;
            return 1;
        }
    }
    return 0;
}

static void fail(hs_t *h, const char *why) {
    if (h->st == HS_FAIL) return;
    snprintf(h->err, sizeof h->err, "%s", why);
    unqueue(h);
    bury(h->c, !h->opened); h->c = NULL;
    h->st = HS_FAIL;
}

static void deliver_open(TCPConnection *c, PDNetErr err) {
    if (dbg_outage_until && (int32_t)(tick_ms() - dbg_outage_until) < 0) err = (PDNetErr)-21;   /* test: connection failed */
    int k = flight_find(c);
    uint32_t took = 0; int was_lost = 0; char host[48] = "?";
    if (k >= 0) {
        took = tick_ms() - fl[k].at; was_lost = fl[k].lost;
        snprintf(host, sizeof host, "%s", fl[k].host);
        fl[k] = fl[--nfl];
    }
    hs_t *h = tcp->getUserdata(c);
    if (h) {
        if (h->c == c) {
            h->opened = 1; h->open_err = err;
            if (err != NET_OK || took > 8000) LOG("net: open %s: err %d after %u ms", host, err, (unsigned)took);
        }
        return;
    }
    LOG("net: late answer from %s (gave up on it): err %d after %u ms%s", host, err, (unsigned)took, was_lost ? ", was stuck" : "");
    for (int i = 0; i < ngrave; i++)
        if (grave[i].c == c && grave[i].st == G_OPENING) grave[i].st = G_CLOSE;
}

static void on_open(TCPConnection *c, PDNetErr err, void *ud) {
    (void)ud;                            /* the hs_t may be long gone: the userdata is cleared when it goes */
    if (c == dbg_hung) return;           /* test hook: this one never answers */
    if (dbg_delay_ms && dbg_nq < FLIGHT_N) {
        int k = flight_find(c);
        uint32_t at = k >= 0 ? fl[k].at : tick_ms();
        dbg_q[dbg_nq].c = c; dbg_q[dbg_nq].err = err; dbg_q[dbg_nq].due = at + (uint32_t)dbg_delay_ms;
        dbg_nq++;
        return;
    }
    deliver_open(c, err);
}
static void on_closed(TCPConnection *c, PDNetErr err) {
    (void)err;
    hs_t *h = tcp->getUserdata(c);
    if (h && h->c == c) h->closed = 1;
}

/* build the request for h->url and queue it; submit() opens the socket */
static int connect_hop(hs_t *h) {
    const char *path;
    if (parse_url(h->url, h->host, sizeof h->host, &h->port, &h->ssl, &path)) { fail(h, "bad url"); return -1; }
    h->c = NULL;
    h->opened = h->open_err = h->closed = 0;
    h->status = 0; h->chunked = 0; h->clen = -1;
    h->hdr_len = 0; h->hdr[0] = 0; h->req_off = 0;
    h->pend_len = h->pend_off = 0;
    h->chunk_left = 0; h->chunk_phase = 0; h->cl_len = 0;
    char range[64] = "";
    if (h->from >= 0) {
        if (h->to >= 0) snprintf(range, sizeof range, "Range: bytes=%lld-%lld\r\n", (long long)h->from, (long long)h->to);
        else snprintf(range, sizeof range, "Range: bytes=%lld-\r\n", (long long)h->from);
    }
    char extra[96] = "";
    if (h->body) snprintf(extra, sizeof extra, "Content-Type: %s\r\nContent-Length: %d\r\n", h->ctype, h->blen);
    int pl = (int)strlen(path);
    if (pl > 4096) pl = 4096;
    h->req_len = snprintf(h->req, sizeof h->req,
        "%s %.*s HTTP/1.1\r\nHost: %s\r\nUser-Agent: " USER_AGENT "\r\n"
        "Accept: */*\r\nAccept-Encoding: identity\r\n%s%sConnection: close\r\n\r\n",
        h->method, pl, path, h->host, range, extra);
    if (h->req_len >= (int)sizeof h->req) { fail(h, "request too long"); return -1; }
    if (nwait == WAIT_N) { fail(h, "too many requests"); return -1; }
    waitq[nwait++] = h;
    h->waiting = 1;
    h->t_queued = h->t_hop = tick_ms();
    h->st = HS_CONNECT;
    return 0;
}

hs_t *hs_open(const char *url, const char *method, int64_t from, int64_t to,
              const char *body, int blen, const char *ctype) {
    hs_t *h = calloc(1, sizeof *h);
    if (!h) return NULL;
    snprintf(h->url, sizeof h->url, "%s", url);
    snprintf(h->method, sizeof h->method, "%s", method ? method : "GET");
    h->from = from; h->to = to;
    h->total = -1; h->range_start = from > 0 ? from : 0;
    if (body) {
        h->body = malloc((size_t)blen + 1);
        if (h->body) { memcpy(h->body, body, (size_t)blen); h->blen = blen; }
        snprintf(h->ctype, sizeof h->ctype, "%s", ctype ? ctype : "application/json");
    }
    h->t_start = tick_ms();
    h->prio = NET_PRIO_NORMAL;
    connect_hop(h);
    return h;
}

void hs_set_priority(hs_t *h, int prio) { if (h) h->prio = prio; }

void hs_close(hs_t *h) {
    if (!h) return;
    unqueue(h);
    bury(h->c, !h->opened);
    free(h->body);
    free(h);
}

static void got_headers(hs_t *h, int hend) {
    sscanf(h->hdr, "HTTP/%*s %d", &h->status);
    static char v[4096];
    if (hs_header(h, "Content-Length", v, sizeof v)) h->clen = atoll(v);
    if (hs_header(h, "Transfer-Encoding", v, sizeof v) && strstr(v, "chunked")) { h->chunked = 1; h->clen = -1; }
    if (h->status >= 300 && h->status < 400 && h->status != 304 && hs_header(h, "Location", v, sizeof v)) {
        static char next[4096];
        resolve(h->url, v, next, sizeof next);
        bury(h->c, 0); h->c = NULL;
        if (++h->hops > MAX_HOPS) { fail(h, "too many redirects"); return; }
        if (!strncmp(h->url, "https://", 8) && !strncmp(next, "http://", 7)) {
            /* allowed (BBC/Triton ends on plain HTTP), just noted */
        }
        snprintf(h->url, sizeof h->url, "%s", next);
        connect_hop(h);
        return;
    }
    char cr[128];
    if (hs_header(h, "Content-Range", cr, sizeof cr)) {      /* bytes a-b/total */
        long long a = 0, b = 0, t = -1;
        if (sscanf(cr, "bytes %lld-%lld/%lld", &a, &b, &t) >= 2) { h->range_start = a; h->total = t; }
    } else if (h->status == 200) {
        h->range_start = 0;
        h->total = h->clen;
    }
    /* body bytes that arrived with the headers */
    int extra = h->hdr_len - hend;
    if (extra > 0) { memcpy(h->pend, h->hdr + hend, (size_t)extra); h->pend_len = extra; h->pend_off = 0; }
    h->hdr[hend] = 0;
    h->st = HS_BODY;
    h->t_last = tick_ms();
}

void hs_poll(hs_t *h) {
    if (!h) return;
    for (int guard = 0; guard < 4; guard++) {
        int before = h->st;
        switch (h->st) {
        case HS_CONNECT:
            if (h->waiting) {
                if (tick_ms() - h->t_queued > QUEUE_WAIT_MS) fail(h, net_stuck() ? "network stuck" : "network busy");
                break;
            }
            if (h->opened) {
                if (h->open_err != NET_OK) { char m[32]; snprintf(m, sizeof m, "connect err %d", h->open_err); fail(h, m); break; }
                h->st = HS_SEND;
                h->t_last = 0;
            } else if (tick_ms() - h->t_hop > (uint32_t)h->connect_ms + 2000) fail(h, "connect timeout");
            break;
        case HS_SEND: {
            /* the header block, then the body; a busy socket means try next frame */
            if (!h->t_last) h->t_last = tick_ms();
            while (h->req_off < h->req_len + h->blen) {
                int hdr = h->req_off < h->req_len;
                const char *src = hdr ? h->req + h->req_off : h->body + (h->req_off - h->req_len);
                size_t left = hdr ? (size_t)(h->req_len - h->req_off) : (size_t)(h->req_len + h->blen - h->req_off);
                int w;
                TIMED("write", w = tcp->write(h->c, src, left));
                if (w == NET_WRITE_BUSY || w == 0) {
                    if (tick_ms() - h->t_last > STALL_MS) fail(h, "send stalled");
                    return;
                }
                if (w < 0) { char m[32]; snprintf(m, sizeof m, "write err %d", w); fail(h, m); return; }
                h->req_off += w;
                h->t_last = tick_ms();
            }
            h->st = HS_HEAD;
            h->t_last = tick_ms();
            break;
        }
        case HS_HEAD: {
            size_t avail;
            TIMED("getBytesAvailable", avail = tcp->getBytesAvailable(h->c));
            if (!avail) {
                if (h->closed) fail(h, "closed before headers");
                else if (tick_ms() - h->t_last > STALL_MS) fail(h, "no response");
                return;
            }
            int room = (int)sizeof h->hdr - 1 - h->hdr_len;
            if (room <= 0) { fail(h, "headers too big"); return; }
            int n;
            TIMED("read", n = tcp->read(h->c, h->hdr + h->hdr_len, avail < (size_t)room ? avail : (size_t)room));
            if (n == NET_READ_TIMEOUT || n == NET_READ_BUSY) return;
            if (n < 0) { char m[32]; snprintf(m, sizeof m, "read err %d", n); fail(h, m); return; }
            if (n == 0) return;
            bytes_total += n;
            h->t_last = tick_ms();
            h->hdr_len += n; h->hdr[h->hdr_len] = 0;
            char *e = strstr(h->hdr, "\r\n\r\n");
            if (e) got_headers(h, (int)(e - h->hdr) + 4);
            break;
        }
        default:
            return;
        }
        if (h->st == before) return;
    }
}

/* raw bytes from the socket into pend; 1 if any, 0 none now, -1 closed */
static int refill(hs_t *h) {
    if (h->pend_off < h->pend_len) return 1;
    h->pend_off = h->pend_len = 0;
    size_t avail;
    TIMED("getBytesAvailable", avail = tcp->getBytesAvailable(h->c));
    if (!avail) return h->closed ? -1 : 0;
    int n;
    TIMED("read", n = tcp->read(h->c, h->pend, avail < sizeof h->pend ? avail : sizeof h->pend));
    if (n == NET_READ_TIMEOUT || n == NET_READ_BUSY) return 0;
    if (n < 0) { snprintf(h->err, sizeof h->err, "read err %d", n); return -2; }
    if (n == 0) return 0;
    bytes_total += n;
    h->pend_len = n;
    h->t_last = tick_ms();
    return 1;
}

int hs_read(hs_t *h, void *buf, int cap) {
    if (!h) return -2;
    if (h->st == HS_DONE) return -1;
    if (h->st == HS_FAIL) return -2;
    if (h->st != HS_BODY) { hs_poll(h); return 0; }
    uint8_t *out = buf;
    int put = 0;
    while (put < cap) {
        if (!h->chunked && h->clen >= 0 && h->got >= h->clen) { h->st = HS_DONE; break; }
        int r = refill(h);
        if (r == 0) break;
        if (r == -2) { char m[64]; snprintf(m, sizeof m, "%s", h->err[0] ? h->err : "read error"); fail(h, m); return put ? put : -2; }
        if (r == -1) {
            if (!h->chunked && h->clen >= 0 && h->got < h->clen) { fail(h, "closed early"); return put ? put : -2; }
            h->st = HS_DONE;
            break;
        }
        uint8_t *p = h->pend + h->pend_off;
        int n = h->pend_len - h->pend_off;
        if (!h->chunked) {
            int k = n < cap - put ? n : cap - put;
            if (h->clen >= 0 && h->got + k > h->clen) k = (int)(h->clen - h->got);
            memcpy(out + put, p, (size_t)k);
            put += k; h->got += k; h->pend_off += k;
            if (h->clen >= 0 && h->got >= h->clen) { h->st = HS_DONE; break; }
            continue;
        }
        /* chunked: phase 0 size line, 1 data, 2 CRLF after data, 3 trailer */
        if (h->chunk_phase == 1) {
            int k = n < cap - put ? n : cap - put;
            if (k > h->chunk_left) k = (int)h->chunk_left;
            memcpy(out + put, p, (size_t)k);
            put += k; h->got += k; h->pend_off += k; h->chunk_left -= k;
            if (h->chunk_left == 0) { h->chunk_phase = 2; h->cl_len = 0; }
            continue;
        }
        char c = (char)*p; h->pend_off++;
        if (c == '\n') {
            h->cl[h->cl_len] = 0;
            if (h->chunk_phase == 0) {
                long long sz = strtoll(h->cl, NULL, 16);
                if (sz == 0) { h->chunk_phase = 3; }
                else { h->chunk_left = sz; h->chunk_phase = 1; }
            } else if (h->chunk_phase == 2) h->chunk_phase = 0;
            else if (h->chunk_phase == 3 && h->cl_len <= 1) { h->st = HS_DONE; bury(h->c, 0); h->c = NULL; break; }
            h->cl_len = 0;
        } else if (h->cl_len < (int)sizeof h->cl - 1) h->cl[h->cl_len++] = c;
    }
    if (dbg_cut_bytes && !dbg_cut_done && h->prio == NET_PRIO_HIGH && h->got > dbg_cut_bytes) {
        dbg_cut_done = 1; dbg_outage_until = tick_ms() + (uint32_t)dbg_outage_ms;
        LOG("net: test: dropping the stream now");
        fail(h, "test drop"); return put ? put : -2;
    }
    if (h->st == HS_BODY && put == 0 && h->c && tick_ms() - h->t_last > STALL_MS) { fail(h, "stalled"); return -2; }
    if (put > 0 && !h->t_first) h->t_first = tick_ms();
    if (put == 0 && h->st == HS_DONE) return -1;
    return put;
}

int hs_state(const hs_t *h) { return h ? h->st : HS_FAIL; }
int hs_status(const hs_t *h) { return h ? h->status : 0; }
int hs_hops(const hs_t *h) { return h ? h->hops : 0; }
const char *hs_url(const hs_t *h) { return h ? h->url : ""; }
const char *hs_error(const hs_t *h) { return h ? h->err : "no request"; }
int64_t hs_total(const hs_t *h) { return h ? h->total : -1; }
int64_t hs_range_start(const hs_t *h) { return h ? h->range_start : 0; }
uint32_t hs_ttfb_ms(const hs_t *h) { return h && h->t_first ? h->t_first - h->t_start : 0; }
uint32_t hs_started(const hs_t *h) { return h ? h->t_start : 0; }
int64_t hs_got(const hs_t *h) { return h ? h->got : 0; }

/* ---------- whole-body fetches ---------- */

int fetch_start(fetch_t *f, const char *url, const char *method, const char *body, int blen, const char *ctype, int max) {
    memset(f, 0, sizeof *f);
    f->max = max;
    f->h = hs_open(url, method, -1, -1, body, blen, ctype);
    if (!f->h) { f->done = -1; return -1; }
    return 0;
}

int fetch_poll(fetch_t *f) {
    if (f->done) return 1;
    hs_poll(f->h);
    int st = hs_state(f->h);
    if (st == HS_FAIL) { f->done = -1; f->status = hs_status(f->h); return 1; }
    if (st != HS_BODY && st != HS_DONE) return 0;
    if (!f->date[0]) hs_header(f->h, "Date", f->date, sizeof f->date);
    for (int guard = 0; guard < 64; guard++) {
        if (f->cap - f->len < 4096) {
            int nc = f->cap ? f->cap * 2 : 16384;
            if (nc > f->max + 1) nc = f->max + 1;
            if (nc <= f->len + 1) { f->done = -1; return 1; }   /* too big */
            char *nb = realloc(f->buf, (size_t)nc);
            if (!nb) { f->done = -1; return 1; }
            f->buf = nb; f->cap = nc;
        }
        int n = hs_read(f->h, f->buf + f->len, f->cap - 1 - f->len);
        if (n > 0) { f->len += n; continue; }
        if (n == 0) return 0;
        f->status = hs_status(f->h);
        if (n == -1) { f->buf[f->len] = 0; f->done = 1; }
        else f->done = -1;
        hs_close(f->h); f->h = NULL;
        return 1;
    }
    return 0;
}

void fetch_free(fetch_t *f) {
    if (f->h) hs_close(f->h);
    free(f->buf);
    memset(f, 0, sizeof *f);
}
