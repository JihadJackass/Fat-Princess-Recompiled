/*
 * ps3recomp - log a diagnostic only the first few times
 */
#ifndef PS3RECOMP_LOG_FIRST_H
#define PS3RECOMP_LOG_FIRST_H

/* True for the first `limit` evaluations of the counter `ctr` (a static variable of the call site), then false:
 *     static int _n = 0; if (PS3_LOG_FIRST(_n, 8)) fprintf(stderr, ...);
 * The counter stops at the limit instead of counting on, so a hot path cannot wrap it past 2^31 and start
 * printing on every call again. */
#define PS3_LOG_FIRST(ctr, limit) ((ctr) < (limit) && ++(ctr))

#include <stdlib.h>
/* PS3_SPURS_VERBOSE=1: keep logging every SPURS task create/run/exit and
 * SendSignal. Off, each of those logs its first 40 and then goes quiet: a
 * level runs ~50k physics tasks, and one locked, flushed line per event made
 * a 19 MB log and slowed every SPU thread. */
static inline int ps3_verbose_spurs(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PS3_SPURS_VERBOSE"); v = (e && *e && *e != '0') ? 1 : 0; }
    return v;
}

#endif /* PS3RECOMP_LOG_FIRST_H */
