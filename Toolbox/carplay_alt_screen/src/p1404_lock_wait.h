#ifndef P1404_LOCK_WAIT_H
#define P1404_LOCK_WAIT_H
#include <errno.h>
#include <unistd.h>

/* QNX priority scheduling can starve the owner if a waiter spins. Sleeping is
 * only for contention and must not replace a socket/libc caller's errno. */
static inline void p1404_lock_wait_yield(void) {
    int saved_errno = errno;
    (void)usleep(1000u);
    errno = saved_errno;
}
#endif
