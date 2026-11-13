/* newlib wants these on the device even though nothing here uses stdio files
 * or signals (snprintf/sscanf pull in the reentrancy plumbing). They're
 * never reached in practice. */
#if TARGET_PLAYDATE
#include <errno.h>
#include <sys/stat.h>
#undef errno
extern int errno;
int _close(int f) { (void)f; errno = EBADF; return -1; }
int _lseek(int f, int o, int w) { (void)f; (void)o; (void)w; return 0; }
int _read(int f, char *p, int n) { (void)f; (void)p; (void)n; return 0; }
int _write(int f, char *p, int n) { (void)f; (void)p; return n; }
int _fstat(int f, struct stat *st) { (void)f; st->st_mode = S_IFCHR; return 0; }
int _isatty(int f) { (void)f; return 1; }
int _kill(int pid, int sig) { (void)pid; (void)sig; errno = EINVAL; return -1; }
int _getpid(void) { return 1; }
void _exit(int status) { (void)status; for (;;) {} }

/* calloc. setup.c routes _malloc_r/_realloc_r/_free_r to the system's
 * allocator but not _calloc_r, and newlib's own _calloc_r takes its clear
 * length from the word before the block (a header of newlib's malloc, not of
 * the one that made the block). So this zeroes exactly what was asked for.
 *
 * It must call _malloc_r, never malloc: GCC turns malloc + memset(0) into
 * calloc, and calloc calls _calloc_r, so 0.1.1 (which used malloc here) spun
 * forever the first time anything called calloc, including printf's %f and
 * strtod (newlib's _Balloc). `make dist` checks the binary for that loop. */
#include <reent.h>
#include <stdlib.h>
#include <string.h>
void *_calloc_r(struct _reent *r, size_t n, size_t size) {
    size_t total;
    if (__builtin_mul_overflow(n, size, &total)) { r->_errno = ENOMEM; return NULL; }
    void *p = _malloc_r(r, total);
    if (p) memset(p, 0, total);
    return p;
}
#endif
