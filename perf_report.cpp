/*
 * perf_report.cpp -- the game's own performance measurement.
 *
 * Built from the always-on accounting in ps3_perfstat.h (where every guest
 * thread's time goes: its own game code, system-library calls, waiting on
 * other threads or the SPUs, polling sleeps), the SPU task time, the host's
 * FIFO drain and present time, frame intervals, and the PC's own CPU load.
 *
 * Three outputs:
 *   - window title, every second: FPS, 1% low, and the busiest guest thread's
 *     split of a frame ("Main 14.1 ms game + 5.0 ms waiting").
 *   - run.log, every 5 s: one "[perf2]" line with the same, in more detail.
 *   - F8 (or FP_PERF_REPORT=<start s>:<seconds>): records 30 s and writes
 *     perf_report.txt next to run.log -- frame-time percentiles, per thread
 *     per frame, top system calls by time, SPU and host costs, CPU load, and a
 *     plain-language reading of what limits the frame rate.
 *
 * Any scene works, including the main menu (which runs a bot match behind
 * it), so no one has to play into a match to measure.
 */
#ifndef NOMINMAX
#  define NOMINMAX   /* windows.h (via ps3_perfstat.h) must not define min/max */
#endif
#include "ps3emu/ps3_perfstat.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <algorithm>
#include <chrono>
#include <vector>
#include <string>
#ifdef _WIN32
#  include <windows.h>
#else
#  include <sys/resource.h>
#  include <unistd.h>
#endif

extern "C" int ppu_prof_snapshot(int, unsigned*, unsigned*, const char**);
extern "C" int ppu_prof_running(int);
extern "C" uint32_t cellGcm_flip_request_count(void);   /* the game's own flips */   /* sys_ppu_thread.c: not finished */
extern "C" const char* ps3_hle_nid_name(uint32_t nid);

/* ---- frame intervals -------------------------------------------------------- */
static double now_ms(void)
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
enum { FR_N = 1 << 14 };
static double s_fr_t[FR_N];
static volatile uint32_t s_fr_n = 0;          /* written by the present thread only */
static double s_host_drain = 0, s_host_present = 0;   /* ms, same thread */

extern "C" void perf_report_frame(void)
{
    s_fr_t[s_fr_n & (FR_N - 1)] = now_ms();
    __atomic_store_n(&s_fr_n, s_fr_n + 1, __ATOMIC_RELEASE);
}
static double s_tsc_per_ms = 0;
extern "C" void perf_report_host(double drain_ms, double present_ms)
{
    s_host_drain += drain_ms; s_host_present += present_ms;
    /* Long drains and presents go to the event ring for the hitch list. */
    const double ms = drain_ms > present_ms ? drain_ms : present_ms;
    if (ms >= 20.0 && s_tsc_per_ms > 0) {
        const uint64_t d = (uint64_t)(ms * s_tsc_per_ms);
        ps3_ps_event(drain_ms > present_ms ? PS3_PS_EV_DRAIN : PS3_PS_EV_PRESENT, 0, 0, ps3_ps_tsc() - d, d);
    }
}

/* ---- CPU load of the whole PC and of this process ----------------------------- */
struct CpuSample { double sys_busy, sys_total, proc; };   /* all in ms of CPU */
static CpuSample cpu_sample(void)
{
    CpuSample c = { 0, 0, 0 };
#ifdef _WIN32
    FILETIME idle, kern, user, cr, ex, pk, pu;
    auto ms = [](FILETIME f) { return (double)(((uint64_t)f.dwHighDateTime << 32) | f.dwLowDateTime) / 1e4; };
    if (GetSystemTimes(&idle, &kern, &user)) {
        c.sys_total = ms(kern) + ms(user);           /* kernel time includes idle */
        c.sys_busy = c.sys_total - ms(idle);
    }
    if (GetProcessTimes(GetCurrentProcess(), &cr, &ex, &pk, &pu)) c.proc = ms(pk) + ms(pu);
#else
    FILE* f = fopen("/proc/stat", "r");
    if (f) {
        unsigned long long v[10] = { 0 };
        if (fscanf(f, "cpu %llu %llu %llu %llu %llu %llu %llu %llu",
                   &v[0], &v[1], &v[2], &v[3], &v[4], &v[5], &v[6], &v[7]) == 8) {
            const double hz = (double)sysconf(_SC_CLK_TCK);
            double tot = 0; for (int i = 0; i < 8; i++) tot += (double)v[i];
            c.sys_total = tot * 1000.0 / hz;
            c.sys_busy = (tot - (double)v[3] - (double)v[4]) * 1000.0 / hz;
        }
        fclose(f);
    }
    struct rusage ru;
    if (!getrusage(RUSAGE_SELF, &ru))
        c.proc = (ru.ru_utime.tv_sec + ru.ru_stime.tv_sec) * 1000.0 +
                 (ru.ru_utime.tv_usec + ru.ru_stime.tv_usec) / 1000.0;
#endif
    return c;
}
static int cpu_count(void)
{
#ifdef _WIN32
    SYSTEM_INFO si; GetSystemInfo(&si); return (int)si.dwNumberOfProcessors;
#else
    long n = sysconf(_SC_NPROCESSORS_ONLN); return n > 0 ? (int)n : 1;
#endif
}

/* ---- snapshots ------------------------------------------------------------------ */
struct Snap {
    double ms; uint64_t tsc;
    uint32_t frames, flips;
    double drain, present;
    CpuSample cpu;
    bool live[PS3_PS_THREADS];
    std::string name[PS3_PS_THREADS];
    ps3_ps_state ps;      /* ~0.5 MB; three of these live at once */
};
static void take(Snap* s)
{
    s->ms = now_ms();
    s->tsc = ps3_ps_tsc();
    s->frames = __atomic_load_n(&s_fr_n, __ATOMIC_ACQUIRE);
    s->flips = cellGcm_flip_request_count();
    s->drain = s_host_drain; s->present = s_host_present;
    s->cpu = cpu_sample();
    for (int i = 0; i < PS3_PS_THREADS; i++) { s->live[i] = false; s->name[i].clear(); }
    for (int i = 0; i < PS3_PS_THREADS; i++) {
        unsigned tid = 0, cia = 0; const char* nm = NULL;
        if (!ppu_prof_snapshot(i, &tid, &cia, &nm) || tid >= PS3_PS_THREADS) continue;
        if (!ppu_prof_running(i)) continue;   /* finished, waiting to be joined: no work */
        s->live[tid] = true;
        s->name[tid] = nm && *nm ? nm : "thread";
    }
    s->tsc = ps3_ps_tsc();
    memcpy(&s->ps, &g_ps3_ps, sizeof s->ps);
    /* Add the calls still in progress up to this instant (see cur_t0). */
    for (int i = 0; i < PS3_PS_THREADS; i++) {
        ps3_ps_thread* t = &s->ps.thr[i];
        const uint64_t t0 = t->cur_t0;
        if (!t0 || t0 >= s->tsc) continue;
        const uint64_t d = s->tsc - t0;
        switch (t->cur_kind) {
        case 0: t->sleep += d; t->sc[t->cur_num] += d; break;
        case 1: t->block += d; if (t->cur_num) t->sc[t->cur_num] += d; break;
        case 2: t->sys += d; t->sc[t->cur_num] += d; break;
        default: t->hle += d; break;
        }
    }
}

struct Thr { unsigned tid; std::string name; double game, hle, sys, block, sleep; };
struct Win {
    double wall_ms = 0, frames = 0, fps = 0, game_fps = 0;
    double ft_avg = 0, ft_p50 = 0, ft_p99 = 0, ft_p999 = 0, ft_max = 0, low1 = 0;
    double over17 = 0, over34 = 0;           /* share of frames */
    std::vector<std::string> silent;         /* threads that made no call at all */
    double drain = 0, present = 0;           /* ms per frame */
    double spu = 0, spu_wait = 0, spu_tasks = 0;   /* per frame */
    double proc_cores = 0, sys_pct = 0, proc_pct = 0;
    double shaders = 0, shader_ms = 0, pipes = 0, pipe_ms = 0;   /* in the whole window */
    std::vector<Thr> thr;                    /* ms per frame, busiest game code first */
    std::vector<std::pair<double, uint32_t>> nids;   /* ms per frame, nid */
};

static Win diff(const Snap& a, const Snap& b)
{
    Win w;
    w.wall_ms = b.ms - a.ms;
    if (w.wall_ms <= 0) return w;
    const double tpm = (double)(b.tsc - a.tsc) / w.wall_ms;
    if (tpm > 0) {
        s_tsc_per_ms = tpm;
        g_ps3_ps.long_ticks = (uint64_t)(20.0 * tpm);   /* arm the 20 ms event ring */
    }
    const double k = s_tsc_per_ms > 0 ? 1.0 / s_tsc_per_ms : 0;
    const uint32_t nf = b.frames - a.frames;
    w.frames = nf;
    w.fps = nf * 1000.0 / w.wall_ms;
    w.game_fps = (double)(b.flips - a.flips) * 1000.0 / w.wall_ms;
    const double per = nf ? 1.0 / nf : 0;

    /* frame intervals in this window (the ring holds the last 16384) */
    std::vector<double> ft;
    const uint32_t lo = (nf > FR_N - 2) ? b.frames - (FR_N - 2) : a.frames;
    for (uint32_t i = lo + 1; i < b.frames; i++)
        ft.push_back(s_fr_t[i & (FR_N - 1)] - s_fr_t[(i - 1) & (FR_N - 1)]);
    if (!ft.empty()) {
        double sum = 0; for (double x : ft) { sum += x; if (x > 17.5) w.over17++; if (x > 34.0) w.over34++; }
        w.ft_avg = sum / ft.size();
        w.over17 /= ft.size(); w.over34 /= ft.size();
        std::sort(ft.begin(), ft.end());
        auto pct = [&](double p) { size_t i = (size_t)(p * (ft.size() - 1) + 0.5); return ft[std::min(i, ft.size() - 1)]; };
        w.ft_p50 = pct(0.50); w.ft_p99 = pct(0.99); w.ft_p999 = pct(0.999); w.ft_max = ft.back();
        /* 1% low: the average frame rate of the slowest 1% of frames */
        size_t n1 = std::max<size_t>(1, ft.size() / 100);
        double s1 = 0; for (size_t i = ft.size() - n1; i < ft.size(); i++) s1 += ft[i];
        w.low1 = s1 > 0 ? 1000.0 * n1 / s1 : 0;
    }
    w.drain = (b.drain - a.drain) * per;
    w.present = (b.present - a.present) * per;
    w.spu = (double)(b.ps.spu_work_ticks - a.ps.spu_work_ticks) * k * per;
    w.spu_wait = (double)(b.ps.spu_wait_ticks - a.ps.spu_wait_ticks) * k * per;
    w.spu_tasks = (double)(b.ps.spu_tasks - a.ps.spu_tasks) * per;
    w.shaders = (double)(b.ps.shader_count - a.ps.shader_count);
    w.shader_ms = (double)(b.ps.shader_ticks - a.ps.shader_ticks) * k;
    w.pipes = (double)(b.ps.pipe_count - a.ps.pipe_count);
    w.pipe_ms = (double)(b.ps.pipe_ticks - a.ps.pipe_ticks) * k;
    const double st = b.cpu.sys_total - a.cpu.sys_total;
    if (st > 0) {
        w.sys_pct = 100.0 * (b.cpu.sys_busy - a.cpu.sys_busy) / st;
        w.proc_pct = 100.0 * (b.cpu.proc - a.cpu.proc) / st;
    }
    w.proc_cores = (b.cpu.proc - a.cpu.proc) / w.wall_ms;

    if (nf) {
        for (unsigned t = 0; t < PS3_PS_THREADS; t++) {
            if (!a.live[t] || !b.live[t]) continue;   /* only threads alive for the whole window */
            const ps3_ps_thread& x = a.ps.thr[t]; const ps3_ps_thread& y = b.ps.thr[t];
            /* No call of any kind in the whole window: a thread that has stopped
             * (or spins without ever calling the system). Not ranked as busy. */
            if (y.sleep == x.sleep && y.block == x.block && y.sys == x.sys && y.hle == x.hle) {
                w.silent.push_back(b.name[t] + " (" + std::to_string(t) + ")");
                continue;
            }
            Thr r;
            r.tid = t; r.name = b.name[t];
            r.sleep = (double)(y.sleep - x.sleep) * k;
            r.block = (double)(y.block - x.block) * k;
            r.sys = (double)(y.sys - x.sys) * k;
            r.hle = (double)(y.hle - x.hle) * k;
            r.game = std::max(0.0, w.wall_ms - r.sleep - r.block - r.sys - r.hle);
            r.sleep *= per; r.block *= per; r.sys *= per; r.hle *= per; r.game *= per;
            w.thr.push_back(r);
        }
        std::sort(w.thr.begin(), w.thr.end(), [](const Thr& p, const Thr& q) { return p.game > q.game; });
        for (int i = 0; i < PS3_PS_NIDS; i++) {
            const ps3_ps_nid& y = b.ps.nid[i];
            if (!y.nid) continue;
            const uint64_t x = a.ps.nid[i].nid == y.nid ? a.ps.nid[i].ticks : 0;
            const double v = (double)(y.ticks - x) * k * per;
            if (v >= 0.01) w.nids.push_back({ v, y.nid });
        }
        std::sort(w.nids.begin(), w.nids.end(), [](auto& p, auto& q) { return p.first > q.first; });
    }
    return w;
}

static const char* sc_name(unsigned n)
{
    switch (n) {
    case 43: return "thread_yield"; case 44: return "thread_join"; case 85: return "event_flag_wait";
    case 92: return "semaphore_wait"; case 97: return "lwmutex_lock"; case 102: return "mutex_lock";
    case 107: return "cond_wait"; case 113: return "lwcond_wait"; case 122: return "rwlock_rlock";
    case 125: return "rwlock_wlock"; case 130: return "event_queue_receive"; case 141: return "usleep";
    case 142: return "sleep"; case 178: return "spu_group_join"; case 801: return "fs_open";
    case 802: return "fs_read"; case 803: return "fs_write"; case 804: return "fs_close";
    case 818: return "fs_lseek"; default: return NULL;
    }
}
static std::string nid_label(uint32_t nid)
{
    /* libsre queue imports run the console's own code and have no handler name */
    switch (nid) {
    case 0x91066667u: return "cellSpursQueuePopBody";
    case 0x92CFF6EDu: return "cellSpursQueuePushBody";
    case 0x082BFB09u: return "_cellSpursQueueInitialize";
    default: break;
    }
    const char* n = ps3_hle_nid_name(nid);
    char b[32];
    if (n && *n && strcmp(n, "?")) return n;
    snprintf(b, sizeof b, "import 0x%08X", nid);
    return b;
}

/* ---- hitches: the longest frames and what overlapped them ----------------------- */
static void write_hitches(FILE* f, const Win& w, const Snap& a, const Snap& b)
{
    (void)w;
    if (s_tsc_per_ms <= 0) return;
    struct H { double t1, len; };
    std::vector<H> hs;
    const uint32_t nf = b.frames - a.frames;
    const uint32_t lo = (nf > FR_N - 2) ? b.frames - (FR_N - 2) : a.frames;
    for (uint32_t i = lo + 1; i < b.frames; i++) {
        const double t1 = s_fr_t[i & (FR_N - 1)], len = t1 - s_fr_t[(i - 1) & (FR_N - 1)];
        if (len >= 50.0) hs.push_back({ t1, len });
    }
    fprintf(f, "HITCHES (frames of 50 ms or more): %zu\n", hs.size());
    if (hs.empty()) { fprintf(f, "\n"); return; }
    std::sort(hs.begin(), hs.end(), [](const H& p, const H& q) { return p.len > q.len; });
    const uint32_t en = b.ps.ev_n;
    const uint32_t first = en > PS3_PS_EVENTS ? en - PS3_PS_EVENTS : 0;
    auto ev_ms = [&](uint64_t tsc) { return b.ms - (double)(int64_t)(b.tsc - tsc) / s_tsc_per_ms; };
    for (size_t h = 0; h < hs.size() && h < 8; h++) {
        const double h0 = hs[h].t1 - hs[h].len, h1 = hs[h].t1;
        fprintf(f, "  %.0f ms frame, %.1f s into the recording\n", hs[h].len, (h0 - a.ms) / 1000.0);
        struct G { uint32_t kind, who, what; double ms; int n; };
        std::vector<G> gs;
        for (uint32_t i = first; i < en; i++) {
            const ps3_ps_ev& e = b.ps.ev[i & (PS3_PS_EVENTS - 1)];
            if (!e.kind) continue;
            const double e0 = ev_ms(e.t0), e1 = e0 + (double)e.dur / s_tsc_per_ms;
            const double ov = std::min(e1, h1) - std::max(e0, h0);
            if (ov <= 0.5) continue;
            /* A thread already waiting when the frame began is idle in the
             * background (a worker parked on its queue), not part of the hitch. */
            if (e.kind <= PS3_PS_EV_IMPORT && e0 < h0 - 5.0) continue;
            bool found = false;
            for (G& g : gs) if (g.kind == e.kind && g.who == e.who && (g.what == e.what || e.kind >= PS3_PS_EV_SHADER)) {
                g.ms += ov; g.n++; found = true; break; }
            if (!found) gs.push_back({ e.kind, e.who, e.what, ov, 1 });
        }
        std::sort(gs.begin(), gs.end(), [](const G& p, const G& q) { return p.ms > q.ms; });
        if (gs.empty()) fprintf(f, "      nothing over 20 ms overlapped it: the time went in game code or many short calls\n");
        for (size_t i = 0; i < gs.size() && i < 5; i++) {
            const G& g = gs[i];
            std::string who = g.who < PS3_PS_THREADS && !b.name[g.who].empty() ? b.name[g.who] : "thread " + std::to_string(g.who);
            std::string what;
            switch (g.kind) {
            case PS3_PS_EV_SYSCALL: { const char* n = sc_name(g.what);
                what = who + " in " + (n ? n : "syscall " + std::to_string(g.what)); break; }
            case PS3_PS_EV_IMPORT: what = who + " in " + nid_label(g.what); break;
            case PS3_PS_EV_SHADER: what = "shader compile x" + std::to_string(g.n); break;
            case PS3_PS_EV_PIPELINE: what = "pipeline build x" + std::to_string(g.n); break;
            case PS3_PS_EV_DRAIN: what = "a long draw-conversion pass"; break;
            case PS3_PS_EV_PRESENT: what = "a long present"; break;
            default: what = "?"; break;
            }
            fprintf(f, "      %6.0f ms  %s\n", g.ms, what.c_str());
        }
    }
    fprintf(f, "\n");
}

/* ---- the report file ----------------------------------------------------------- */
static void write_report(const Win& w, const Snap& a, const Snap& b, const char* path)
{
    FILE* f = fopen(path, "w");
    if (!f) { fprintf(stderr, "[perf2] cannot write %s\n", path); return; }
    const double k = s_tsc_per_ms > 0 ? 1.0 / s_tsc_per_ms : 0;
    const double per = w.frames ? 1.0 / w.frames : 0;
    fprintf(f, "Fat Princess performance report\n");
    fprintf(f, "Recorded %.1f s, %.0f frames. Times are milliseconds per frame unless marked.\n\n", w.wall_ms / 1000.0, w.frames);

    fprintf(f, "FRAME RATE\n");
    fprintf(f, "  average        %6.1f FPS   (%.2f ms a frame)\n", w.fps, w.ft_avg);
    fprintf(f, "  check: the game asked for %.1f new frames a second; the two numbers should match\n", w.game_fps);
    fprintf(f, "  1%% low         %6.1f FPS\n", w.low1);
    fprintf(f, "  frame time     median %.1f   99th pct %.1f   99.9th pct %.1f   worst %.1f\n", w.ft_p50, w.ft_p99, w.ft_p999, w.ft_max);
    fprintf(f, "  The game paces itself to 30 FPS (two refreshes a frame), as on the PS3.\n");
    fprintf(f, "  frames over 34 ms (late for 30 FPS): %.1f%%   (over 17.5 ms: %.1f%%)\n\n", w.over34 * 100, w.over17 * 100);

    fprintf(f, "GUEST THREADS (the game's own PS3 threads), per frame\n");
    fprintf(f, "  %-26s %7s %7s %7s %7s %7s\n", "thread", "game", "syslib", "lv2", "waiting", "sleep");
    int shown = 0;
    for (const Thr& t : w.thr) {
        if (shown >= 12) break;
        if (t.game + t.hle + t.sys + t.block < 0.2) continue;
        char nm[40]; snprintf(nm, sizeof nm, "%s (%u)", t.name.c_str(), t.tid);
        fprintf(f, "  %-26s %7.2f %7.2f %7.2f %7.2f %7.2f\n", nm, t.game, t.hle, t.sys, t.block, t.sleep);
        shown++;
    }
    if (!w.silent.empty()) {
        fprintf(f, "  Made no calls at all in the window (stopped, or spinning without calling the system):");
        for (const std::string& s : w.silent) fprintf(f, " %s", s.c_str());
        fprintf(f, "\n");
    }
    fprintf(f, "  game = running the game's own code, or not scheduled by Windows; syslib = inside a\n"
               "  system-library call; lv2 = other kernel calls; waiting = blocked on another thread\n"
               "  or the SPUs; sleep = polling sleeps.\n\n");

    for (int r = 0; r < 2 && r < (int)w.thr.size(); r++) {
        const unsigned t = w.thr[r].tid;
        std::vector<std::pair<double, unsigned>> sc;
        for (unsigned n = 0; n < PS3_PS_SC; n++) {
            const double v = (double)(b.ps.thr[t].sc[n] - a.ps.thr[t].sc[n]) * k * per;
            if (v >= 0.05) sc.push_back({ v, n });
        }
        std::sort(sc.begin(), sc.end(), [](auto& p, auto& q) { return p.first > q.first; });
        fprintf(f, "  kernel calls of %s (%u):", w.thr[r].name.c_str(), t);
        if (sc.empty()) fprintf(f, " none over 0.05 ms");
        for (size_t i = 0; i < sc.size() && i < 5; i++) {
            const char* n = sc_name(sc[i].second);
            if (n) fprintf(f, "  %s %.2f", n, sc[i].first);
            else fprintf(f, "  syscall %u %.2f", sc[i].second, sc[i].first);
        }
        fprintf(f, "\n");
    }
    fprintf(f, "\nSYSTEM-LIBRARY CALLS BY TIME (all threads), per frame\n");
    for (size_t i = 0; i < w.nids.size() && i < 12; i++)
        fprintf(f, "  %7.2f  %s\n", w.nids[i].first, nid_label(w.nids[i].second).c_str());
    fprintf(f, "\nSPU TASKS (physics and other jobs; each runs on its own host thread)\n");
    fprintf(f, "  %.1f task starts a frame, %.2f ms of task work (all SPU threads together), plus %.2f ms\n"
               "  of tasks idling until signalled\n", w.spu_tasks, w.spu, w.spu_wait);
    { /* which SPU programs the task time goes to */
        const double k = s_tsc_per_ms > 0 ? 1.0 / s_tsc_per_ms : 0, per = w.frames ? 1.0 / w.frames : 0;
        std::vector<std::pair<double, int>> im;
        for (int i = 0; i < 64; i++) {
            const double v = (double)(b.ps.spu_img_work[i] - a.ps.spu_img_work[i]) * k * per;
            if (v >= 0.1) im.push_back({ v, i });
        }
        std::sort(im.begin(), im.end(), [](auto& p, auto& q) { return p.first > q.first; });
        fprintf(f, "  by SPU program (image number: work ms a frame, task starts a frame):");
        for (size_t i = 0; i < im.size() && i < 8; i++)
            fprintf(f, "  #%d: %.1f, %.1f", im[i].second, im[i].first,
                    (double)(b.ps.spu_img_n[im[i].second] - a.ps.spu_img_n[im[i].second]) * per);
        fprintf(f, "\n\n");
    }
    fprintf(f, "RENDERING (frame clock thread)\n");
    fprintf(f, "  FIFO drain (draw conversion and GPU submit) %.2f ms   present %.2f ms\n", w.drain, w.present);
    fprintf(f, "  New shaders compiled: %.0f (%.0f ms in all). New pipelines built: %.0f (%.0f ms in all).\n"
               "  Both happen on the drain, so the game waits for them; with the shader cache they\n"
               "  should drop to zero after the first run.\n\n", w.shaders, w.shader_ms, w.pipes, w.pipe_ms);
    write_hitches(f, w, a, b);
    fprintf(f, "PC\n");
    fprintf(f, "  %d logical CPUs. Whole PC busy %.0f%%. This game %.0f%% of the PC (%.1f cores).\n\n",
            cpu_count(), w.sys_pct, w.proc_pct, w.proc_cores);

    fprintf(f, "READING\n");
    /* The game's own pacing: its vblank handler releases a frame every second
     * refresh, so 30 FPS (33.4 ms) is the target, as on the console. */
    const double budget = 2000.0 / 59.94;
    if (w.fps >= 29.3 && w.over34 < 0.02)
        fprintf(f, "  Holding the game's 30 FPS here.\n");
    else
        fprintf(f, "  %.1f ms a frame on average; the game's 30 FPS needs %.1f ms.\n", w.ft_avg, budget);
    /* The costs that sit on the frame's critical path, largest first. The
     * game's main loop, its render thread, the drain and the present run one
     * after another each frame, so the largest one is the first to fix. */
    std::vector<std::pair<double, std::string>> costs;
    for (size_t i = 0; i < w.thr.size() && i < 3; i++) {
        const Thr& t = w.thr[i];
        costs.push_back({ t.game + t.hle + t.sys, "guest thread " + t.name + " working (game code and system calls)" });
    }
    costs.push_back({ w.drain, "draw conversion and GPU submit (FIFO drain)" });
    costs.push_back({ w.present, "present (waiting for the GPU and the display)" });
    std::sort(costs.begin(), costs.end(), [](auto& p, auto& q) { return p.first > q.first; });
    fprintf(f, "  Largest costs in a frame:\n");
    for (size_t i = 0; i < costs.size() && i < 4; i++)
        fprintf(f, "    %6.1f ms  %s\n", costs[i].first, costs[i].second.c_str());
    if (w.spu > 0)
        fprintf(f, "    (SPU task work, %.1f ms a frame, runs on its own threads alongside these)\n", w.spu);
    if (!w.thr.empty() && w.thr[0].block > budget * 0.25)
        fprintf(f, "  %s also waits %.1f ms a frame on other threads or SPU tasks.\n",
                w.thr[0].name.c_str(), w.thr[0].block);
    if (w.sys_pct > 90) {
        if (w.sys_pct - w.proc_pct > 15)
            fprintf(f, "  The PC is %.0f%% busy and other programs use %.0f%% of it: they compete with the game\n"
                       "  for CPU time, so the times above are longer than the game alone needs.\n",
                    w.sys_pct, w.sys_pct - w.proc_pct);
        else
            fprintf(f, "  The game keeps every CPU core busy (%.0f%%), so its threads compete with each other;\n"
                       "  thread times above include time spent ready to run but not running.\n", w.sys_pct);
    }
    fclose(f);
    fprintf(stderr, "[perf2] wrote %s (%.1f FPS avg, 1%% low %.1f)\n", path, w.fps, w.low1);
}

/* ---- once a second ------------------------------------------------------------ */
static Snap* s_sec = nullptr;    /* last second */
static Snap* s_five = nullptr;   /* last 5 s */
static Snap* s_rec = nullptr;    /* F8 recording start */
static double s_rec_until = 0, s_rec_secs = 0;
static int s_rec_pending = 0;    /* seconds requested */
static double s_auto_start = -1, s_t0 = 0;

extern "C" void perf_report_request(int seconds)
{
    if (s_rec_until > 0) { fprintf(stderr, "[perf2] already recording\n"); return; }
    s_rec_pending = seconds > 0 ? seconds : 30;
}

/* Fills `out` with the title addition ("" when nothing to show). */
extern "C" void perf_report_second(char* out, size_t n)
{
    if (n) out[0] = 0;
    if (!s_sec) {
        s_sec = new Snap(); s_five = new Snap();
        take(s_sec); *s_five = *s_sec; s_t0 = s_sec->ms;
        const char* e = getenv("FP_PERF_REPORT");          /* <start s>:<seconds> */
        double st = 0, len = 30;
        if (e && sscanf(e, "%lf:%lf", &st, &len) >= 1) { s_auto_start = st; s_rec_secs = len; }
        return;
    }
    Snap* cur = new Snap();
    take(cur);
    const Win w = diff(*s_sec, *cur);

    if (s_auto_start >= 0 && cur->ms - s_t0 >= s_auto_start * 1000.0) {
        s_auto_start = -1;
        perf_report_request((int)s_rec_secs);
    }
    if (s_rec_pending && !s_rec) {
        s_rec = new Snap(); *s_rec = *cur;
        s_rec_until = cur->ms + s_rec_pending * 1000.0;
        fprintf(stderr, "[perf2] recording %d s for perf_report.txt\n", s_rec_pending);
        s_rec_pending = 0;
    }

    if (n) {
        if (!w.thr.empty() && w.frames) {
            const Thr& t = w.thr[0];
            snprintf(out, n, "1%% low %.0f  |  %s %.1f ms work + %.1f wait / frame  |  PC CPU %.0f%%%s",
                     w.low1, t.name.c_str(), t.game + t.hle + t.sys, t.block + t.sleep, w.sys_pct,
                     s_rec ? "" : "  |  F8: report");
        }
        if (s_rec) {
            char rec[64];
            snprintf(rec, sizeof rec, "  |  recording %.0f s", (s_rec_until - cur->ms) / 1000.0 + 0.5);
            strncat(out, rec, n - strlen(out) - 1);
        }
    }

    if (cur->ms - s_five->ms >= 4999.0) {
        const Win f = diff(*s_five, *cur);
        char thr[512] = "";
        for (size_t i = 0; i < f.thr.size() && i < 3; i++) {
            char one[160];
            snprintf(one, sizeof one, " | %s game=%.1f lib=%.1f lv2=%.1f wait=%.1f sleep=%.1f",
                     f.thr[i].name.c_str(), f.thr[i].game, f.thr[i].hle, f.thr[i].sys, f.thr[i].block, f.thr[i].sleep);
            strncat(thr, one, sizeof thr - strlen(thr) - 1);
        }
        fprintf(stderr, "[perf2] fps=%.1f low1=%.1f ft p50=%.1f p99=%.1f max=%.1f miss=%.0f%% | drain=%.2f present=%.2f "
                        "spu=%.2f (wait %.2f, %.0f tasks) | new shaders=%.0f pipelines=%.0f | cpu pc=%.0f%% game=%.1f cores%s (ms/frame)\n",
                f.fps, f.low1, f.ft_p50, f.ft_p99, f.ft_max, f.over17 * 100, f.drain, f.present,
                f.spu, f.spu_wait, f.spu_tasks, f.shaders, f.pipes, f.sys_pct, f.proc_cores, thr);
        *s_five = *cur;
    }

    if (s_rec && cur->ms >= s_rec_until) {
        const Win r = diff(*s_rec, *cur);
        write_report(r, *s_rec, *cur, "perf_report.txt");
        delete s_rec; s_rec = nullptr; s_rec_until = 0;
    }
    std::swap(s_sec, cur);
    delete cur;
}
