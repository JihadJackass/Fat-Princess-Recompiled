/*
 * ps3_perfstat -- always-on, low-cost time accounting for the performance
 * report (perf_report.cpp): where each guest thread's time goes, frame by
 * frame, without a recorder or a profile build.
 *
 * Per guest thread (thread_id < PS3_PS_THREADS), the owning thread adds CPU
 * timestamp-counter ticks to its own slot as it leaves:
 *   sleep - sys_timer_usleep / sleep / yield (polling: "nothing to do yet")
 *   block - waiting on another thread or the SPUs (semaphore, lwmutex, cond,
 *           event queue, event flag, join)
 *   sys   - every other lv2 syscall
 *   hle   - time inside system-library imports (ps3_hle_call), which can also
 *           block; syscalls made from inside an import count here, not twice
 * plus the per-syscall split and a per-import table. SPU task threads add
 * their run time and the part of it spent waiting for a signal.
 *
 * The reporter reads all of this racily once a second and works in deltas, so
 * no locks; a torn 64-bit read on x64 cannot happen. Cost per syscall/import:
 * two rdtsc and a few adds.
 *
 * Header-only and shared through one weak/selectany object, like ps3_ftrace.h,
 * so the hooked files still compile on their own in the unit tests.
 */
#ifndef PS3EMU_PS3_PERFSTAT_H
#define PS3EMU_PS3_PERFSTAT_H

#include <stdint.h>
#if defined(_MSC_VER) || defined(__x86_64__) || defined(_M_X64) || defined(__i386__)
#  if defined(_MSC_VER)
#    include <intrin.h>
#  else
#    include <x86intrin.h>
#  endif
#  define PS3_PS_HAVE_TSC 1
#else
#  include <time.h>
#endif
#ifdef _WIN32
#  include <windows.h>
#else
#  include <pthread.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

#if defined(_MSC_VER)
#  define PS3_PS_SHARED __declspec(selectany)
#else
#  define PS3_PS_SHARED __attribute__((weak))
#endif

enum { PS3_PS_THREADS = 64, PS3_PS_SC = 1024, PS3_PS_NIDS = 1024 };

typedef struct {
    uint64_t sleep, block, sys, hle;
    uint64_t in_hle;                 /* host thread inside an import right now, or 0 (a
                                      * thread that died inside one leaves a stale id,
                                      * which then matches no thread) */
    /* The call in progress, so a thread blocked for seconds shows as waiting
     * now rather than when the call returns: the reporter adds now - cur_t0 to
     * its class in each snapshot. cur_kind: 0 sleep, 1 block, 2 sys, 3 hle. */
    uint64_t cur_t0;
    uint32_t cur_kind, cur_num;
    uint64_t sc[PS3_PS_SC];          /* ticks per syscall number */
} ps3_ps_thread;

typedef struct { uint32_t nid, calls; uint64_t ticks; } ps3_ps_nid;

/* A long event, for naming what happened during a hitch: any wait or call of
 * 20 ms or more, every shader compile and pipeline build, long drains and
 * presents. kind: 1 lv2 syscall (what = number), 2 import (what = NID),
 * 3 shader compile, 4 pipeline build, 5 FIFO drain pass, 6 present.
 * who = guest thread id, or 0 for host work. */
enum { PS3_PS_EV_SYSCALL = 1, PS3_PS_EV_IMPORT, PS3_PS_EV_SHADER, PS3_PS_EV_PIPELINE,
       PS3_PS_EV_DRAIN, PS3_PS_EV_PRESENT };
enum { PS3_PS_EVENTS = 4096 };
typedef struct { uint64_t t0, dur; uint32_t kind, who, what, pad; } ps3_ps_ev;

typedef struct {
    ps3_ps_thread thr[PS3_PS_THREADS];
    ps3_ps_nid nid[PS3_PS_NIDS];
    uint64_t spu_ticks, spu_wait_ticks, spu_tasks;   /* atomic adds */
    uint64_t spu_work_ticks;   /* task time outside signal waits, added as it happens */
    uint64_t shader_ticks, shader_count, pipe_ticks, pipe_count;   /* render thread only */
    uint64_t spu_img_work[64], spu_img_n[64];   /* per lifted SPU image: work ticks, task starts */
    uint64_t long_ticks;       /* event threshold in ticks; 0 until the reporter sets it */
    uint32_t ev_n;
    ps3_ps_ev ev[PS3_PS_EVENTS];
} ps3_ps_state;

PS3_PS_SHARED ps3_ps_state g_ps3_ps = { 0 };

static inline uint64_t ps3_ps_tsc(void)
{
#ifdef PS3_PS_HAVE_TSC
    return __rdtsc();
#else
    struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts);
    return (uint64_t)ts.tv_sec * 1000000000ull + (uint64_t)ts.tv_nsec;
#endif
}

static inline void ps3_ps_event(uint32_t kind, uint32_t who, uint32_t what, uint64_t t0, uint64_t dur)
{
    const uint32_t i = __atomic_fetch_add(&g_ps3_ps.ev_n, 1u, __ATOMIC_RELAXED);
    ps3_ps_ev* e = &g_ps3_ps.ev[i & (PS3_PS_EVENTS - 1)];
    e->t0 = t0; e->dur = dur; e->kind = kind; e->who = who; e->what = what;
}
static inline int ps3_ps_is_long(uint64_t d)
{
    const uint64_t lt = g_ps3_ps.long_ticks;
    return lt && d >= lt;
}

/* System-library imports that block until another thread or the SPUs do
 * something; their time counts as waiting, not as library work. */
static inline int ps3_ps_nid_waits(uint32_t nid)
{
    switch (nid) {
    case 0x373523D4u:   /* cellSpursEventFlagWait */
    case 0xA7C066DEu:   /* cellSpursJoinJobChain */
    case 0x9F72ADD3u:   /* cellSpursJoinTaskset */
    case 0x1573DC3Fu:   /* sys_lwmutex_lock */
    case 0x91066667u:   /* cellSpursQueuePopBody */
        return 1;
    default: return 0;
    }
}

/* lv2 syscall classes for the split above. */
static inline int ps3_ps_sc_class(uint32_t num)
{
    switch (num) {
    case 141: case 142: case 43: return 0;                  /* usleep, sleep, yield */
    case 44: case 85: case 92: case 97: case 102: case 107:
    case 113: case 122: case 125: case 130: case 178: return 1;
    default: return 2;
    }
}

static inline void ps3_ps_syscall_done(uint32_t tid, uint32_t num, uint64_t t0)
{
    if (tid >= PS3_PS_THREADS || !t0) return;
    ps3_ps_thread* t = &g_ps3_ps.thr[tid];
    const uint64_t d = ps3_ps_tsc() - t0;
    switch (ps3_ps_sc_class(num)) {
    case 0: t->sleep += d; break;
    case 1: t->block += d; break;
    default: t->sys += d; break;
    }
    t->sc[num & (PS3_PS_SC - 1)] += d;
    if (ps3_ps_sc_class(num) != 0 && ps3_ps_is_long(d)) ps3_ps_event(PS3_PS_EV_SYSCALL, tid, num, t0, d);
    __atomic_store_n(&t->cur_t0, 0, __ATOMIC_RELEASE);
}

/* Returns the start stamp, or 0 when this call is nested in an import (its
 * time already counts there). */
static inline uint64_t ps3_ps_host_tid(void)
{
#ifdef _WIN32
    return (uint64_t)GetCurrentThreadId();
#else
    return (uint64_t)(uintptr_t)pthread_self();
#endif
}

static inline uint64_t ps3_ps_syscall_begin(uint32_t tid, uint32_t num)
{
    if (tid >= PS3_PS_THREADS || g_ps3_ps.thr[tid].in_hle == ps3_ps_host_tid()) return 0;
    ps3_ps_thread* t = &g_ps3_ps.thr[tid];
    const uint64_t t0 = ps3_ps_tsc();
    t->cur_kind = (uint32_t)ps3_ps_sc_class(num); t->cur_num = num & (PS3_PS_SC - 1);
    __atomic_store_n(&t->cur_t0, t0, __ATOMIC_RELEASE);
    return t0;
}

static inline void ps3_ps_nid_add(uint32_t nid, uint64_t d)
{
    uint32_t h = (nid * 2654435761u) >> 22;
    for (uint32_t i = 0; i < PS3_PS_NIDS; i++) {
        ps3_ps_nid* e = &g_ps3_ps.nid[(h + i) & (PS3_PS_NIDS - 1)];
        uint32_t cur = __atomic_load_n(&e->nid, __ATOMIC_RELAXED);
        if (cur != nid) {
            if (cur) continue;
            uint32_t z = 0;
            if (!__atomic_compare_exchange_n(&e->nid, &z, nid, 0, __ATOMIC_ACQ_REL, __ATOMIC_RELAXED) && z != nid)
                continue;
        }
        __atomic_add_fetch(&e->calls, 1u, __ATOMIC_RELAXED);
        __atomic_add_fetch(&e->ticks, d, __ATOMIC_RELAXED);
        return;
    }
}

#ifdef __cplusplus
}
#endif
#endif
