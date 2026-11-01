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
#endif
