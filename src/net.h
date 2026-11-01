#pragma once
#include <stdint.h>

/* HTTP/1.1 over the SDK's TLS TCP sockets, driven from the update loop.
 *
 * Why not the SDK's http API: it follows redirects in the background even
 * though it hands back the Location, and those hidden requests hold slots and
 * leak headers and bytes into later connections; its status is always 0. So
 * we speak HTTP ourselves (like the Chumby client): our own request with
 * Range and Connection: close, our own redirect walk, every socket ours. */

typedef struct hs hs_t;

enum { HS_CONNECT, HS_SEND, HS_HEAD, HS_BODY, HS_DONE, HS_FAIL };

/* from < 0: no Range header. to < 0: open-ended range. body may be NULL. */
hs_t *hs_open(const char *url, const char *method, int64_t from, int64_t to,
              const char *body, int blen, const char *ctype);
/* Advance connect/send/headers/redirects. Call every frame until HS_BODY. */
void hs_poll(hs_t *h);
/* Body bytes (de-chunked): > 0 copied, 0 nothing yet, -1 end of body, -2 error. */
int hs_read(hs_t *h, void *buf, int cap);
void hs_close(hs_t *h);

int hs_state(const hs_t *h);
int hs_status(const hs_t *h);
int hs_hops(const hs_t *h);
const char *hs_url(const hs_t *h);         /* the URL the body came from */
const char *hs_error(const hs_t *h);
int64_t hs_total(const hs_t *h);           /* file size from Content-Range / Length, -1 unknown */
int64_t hs_range_start(const hs_t *h);     /* first byte of the body within the file */
uint32_t hs_ttfb_ms(const hs_t *h);        /* first body byte, ms after hs_open (0 until then) */
uint32_t hs_started(const hs_t *h);        /* tick_ms at hs_open */
int64_t hs_got(const hs_t *h);
int hs_header(const hs_t *h, const char *name, char *v, int cap);

/* Whole-body GET/POST into memory (schedule, counts, reports). */
typedef struct {
    hs_t *h;
    char *buf; int len, cap, max;
    int done, status;          /* done: 1 ok, -1 failed */
    char date[40];             /* the response's Date header (a clock source) */
} fetch_t;
int fetch_start(fetch_t *f, const char *url, const char *method, const char *body, int blen, const char *ctype, int max);
/* 1 when finished (check f->done), 0 still going. */
int fetch_poll(fetch_t *f);
void fetch_free(fetch_t *f);

/* Close sockets later: releasing straight after close() wedged the
 * simulator's network stack in the spike. Call every frame. */
void net_tick(void);
void net_init(void);
int net_open_count(void);
/* Bytes received over all sockets since start (for throughput stats). */
int64_t net_bytes_total(void);
