/* Built-in sampling profiler (Windows).
 *
 * FP_PROFILE=1 (fatprincess.ini [Performance] Profile=1, or the launcher's
 * Performance tab) starts a thread that, once a millisecond, looks at every
 * thread of the process that used CPU since its last look, stops it for a
 * moment and records where it was. Every 30 seconds it appends a report to
 * profile.txt beside the game: per thread its CPU use over the window, its
 * entry point, and its hottest code addresses as exe+RVA (or dll+offset).
 * The addresses are resolved against FatPrincessRecomp.map, which the build
 * writes next to the exe.
 *
 * Nothing is allocated while a thread is suspended (it may hold the heap
 * lock): the sample is stored after the thread has been resumed.
 */
#ifdef _WIN32
#include <windows.h>
#include <tlhelp32.h>
#include <psapi.h>

#include <algorithm>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string>
#include <unordered_map>
#include <vector>

#pragma comment(lib, "psapi.lib")

namespace {

struct ProfThread {
    DWORD    tid;
    HANDLE   h;
    ULONG64  cycles;          /* QueryThreadCycleTime at the last look */
    ULONG64  cpu_100ns;       /* user + kernel time at the window start */
    uint64_t start;           /* thread entry address */
    char     name[64];
    unsigned samples;         /* this window */
    bool     alive;
};

struct Sample { uint32_t thread; uint64_t rip; };

std::vector<ProfThread> g_threads;
std::vector<Sample>     g_samples;
const size_t            kMaxSamples = 4u << 20;

typedef HRESULT (WINAPI *GetDescFn)(HANDLE, PWSTR*);
typedef LONG (NTAPI *NtQitFn)(HANDLE, ULONG, PVOID, ULONG, PULONG);
GetDescFn g_get_desc;
NtQitFn   g_ntqit;

ULONG64 thread_cpu(HANDLE h)
{
    FILETIME c, e, k, u;
    if (!GetThreadTimes(h, &c, &e, &k, &u)) return 0;
    return (((ULONG64)k.dwHighDateTime << 32) | k.dwLowDateTime) +
           (((ULONG64)u.dwHighDateTime << 32) | u.dwLowDateTime);
}

void refresh_threads(DWORD self)
{
    for (auto& t : g_threads) t.alive = false;
    /* Threads that have exited stay listed until the next report (their CPU
     * time is still readable through the handle); then they are dropped. */
    HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE) return;
    THREADENTRY32 te; te.dwSize = sizeof te;
    const DWORD pid = GetCurrentProcessId();
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
        if (te.th32OwnerProcessID != pid || te.th32ThreadID == self) continue;
        auto it = std::find_if(g_threads.begin(), g_threads.end(),
                               [&](const ProfThread& t) { return t.tid == te.th32ThreadID; });
        if (it != g_threads.end()) { it->alive = true; continue; }
        HANDLE h = OpenThread(THREAD_SUSPEND_RESUME | THREAD_GET_CONTEXT | THREAD_QUERY_INFORMATION,
                              FALSE, te.th32ThreadID);
        if (!h) continue;
        ProfThread t = {};
        t.tid = te.th32ThreadID; t.h = h; t.alive = true;
        QueryThreadCycleTime(h, &t.cycles);
        t.cpu_100ns = thread_cpu(h);
        if (g_ntqit) { PVOID a = NULL; if (g_ntqit(h, 9 /* ThreadQuerySetWin32StartAddress */, &a, sizeof a, NULL) == 0) t.start = (uint64_t)a; }
        if (g_get_desc) {
            PWSTR w = NULL;
            if (SUCCEEDED(g_get_desc(h, &w)) && w) {
                WideCharToMultiByte(CP_UTF8, 0, w, -1, t.name, sizeof t.name, NULL, NULL);
                LocalFree(w);
            }
        }
        g_threads.push_back(t);
    }
    CloseHandle(snap);
}

/* "FatPrincessRecomp.exe+0x123456" style location. */
std::string where(uint64_t a)
{
    HMODULE m = NULL;
    char buf[160];
    if (a && GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                (LPCSTR)a, &m) && m) {
        char path[MAX_PATH] = "?";
        GetModuleFileNameA(m, path, sizeof path);
        const char* base = strrchr(path, '\\'); base = base ? base + 1 : path;
        snprintf(buf, sizeof buf, "%s+0x%llx", base, (unsigned long long)(a - (uint64_t)m));
    } else {
        snprintf(buf, sizeof buf, "0x%llx", (unsigned long long)a);
    }
    return buf;
}

void report(FILE* f, double window_s, unsigned window_no)
{
    std::vector<size_t> order;
    std::vector<ULONG64> used(g_threads.size());
    for (size_t i = 0; i < g_threads.size(); i++) {
        const ULONG64 now = thread_cpu(g_threads[i].h);
        used[i] = now - g_threads[i].cpu_100ns;
        g_threads[i].cpu_100ns = now;
        order.push_back(i);
    }
    std::sort(order.begin(), order.end(), [&](size_t a, size_t b) { return used[a] > used[b]; });
    std::vector<std::unordered_map<uint64_t, unsigned>> hist(g_threads.size());
    for (const Sample& s : g_samples) hist[s.thread][s.rip]++;

    fprintf(f, "\n=== window %u: %.1f s, %zu samples, %zu threads ===\n",
            window_no, window_s, g_samples.size(), g_threads.size());
    for (size_t i : order) {
        const ProfThread& t = g_threads[i];
        const double pct = window_s > 0 ? (double)used[i] / (window_s * 1e7) * 100.0 : 0.0;
        if (pct < 1.0 && !t.samples) continue;
        fprintf(f, "thread %lu cpu %.1f%% samples %u name \"%s\" entry %s\n",
                (unsigned long)t.tid, pct, t.samples, t.name, where(t.start).c_str());
    }
    for (size_t i : order) {
        const ProfThread& t = g_threads[i];
        const double pct = window_s > 0 ? (double)used[i] / (window_s * 1e7) * 100.0 : 0.0;
        if (pct < 3.0 || hist[i].empty()) continue;
        std::vector<std::pair<uint64_t, unsigned>> v(hist[i].begin(), hist[i].end());
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
        fprintf(f, "--- thread %lu \"%s\" (%.1f%% cpu): top addresses ---\n",
                (unsigned long)t.tid, t.name, pct);
        for (size_t k = 0; k < v.size() && k < 400; k++)
            fprintf(f, "%u %s\n", v[k].second, where(v[k].first).c_str());
    }
    /* Threads sharing a name (the SPU task threads are all "spu job"): their
     * hottest addresses together, since each alone may be too small to list. */
    {
        std::unordered_map<std::string, std::pair<double, std::unordered_map<uint64_t, unsigned>>> byname;
        std::unordered_map<std::string, unsigned> count;
        for (size_t i = 0; i < g_threads.size(); i++) {
            if (!g_threads[i].name[0]) continue;
            const double pct = window_s > 0 ? (double)used[i] / (window_s * 1e7) * 100.0 : 0.0;
            auto& g = byname[g_threads[i].name];
            g.first += pct;
            for (const auto& kv : hist[i]) g.second[kv.first] += kv.second;
            count[g_threads[i].name]++;
        }
        for (auto& kv : byname) {
            if (count[kv.first] < 2 || kv.second.first < 3.0) continue;
            std::vector<std::pair<uint64_t, unsigned>> v(kv.second.second.begin(), kv.second.second.end());
            std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.second > b.second; });
            fprintf(f, "--- group \"%s\" (%u threads, %.1f%% cpu): top addresses ---\n",
                    kv.first.c_str(), count[kv.first], kv.second.first);
            for (size_t k = 0; k < v.size() && k < 400; k++)
                fprintf(f, "%u %s\n", v[k].second, where(v[k].first).c_str());
        }
    }
    /* Short-lived threads (one per SPU task): their totals by entry point and
     * name, since each alone is too small to list. */
    {
        std::unordered_map<std::string, std::pair<double, unsigned>> groups;
        for (size_t i = 0; i < g_threads.size(); i++) {
            const double pct = window_s > 0 ? (double)used[i] / (window_s * 1e7) * 100.0 : 0.0;
            if (g_threads[i].alive) continue;
            auto& gr = groups[std::string(g_threads[i].name) + " @ " + where(g_threads[i].start)];
            gr.first += pct; gr.second++;
        }
        for (const auto& kv : groups)
            fprintf(f, "exited threads: %u x \"%s\" cpu %.1f%% total\n", kv.second.second, kv.first.c_str(), kv.second.first);
    }
    fflush(f);
    g_samples.clear();
    for (auto& t : g_threads) t.samples = 0;
    /* Drop the exited threads now they have been reported. */
    {
        std::vector<ProfThread> keep;
        for (auto& t : g_threads) { if (t.alive) keep.push_back(t); else CloseHandle(t.h); }
        g_threads.swap(keep);
    }
}

DWORD WINAPI profiler_thread(LPVOID)
{
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
    HMODULE k32 = GetModuleHandleA("kernel32.dll");
    g_get_desc = (GetDescFn)GetProcAddress(k32, "GetThreadDescription");
    g_ntqit = (NtQitFn)GetProcAddress(GetModuleHandleA("ntdll.dll"), "NtQueryInformationThread");
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    FILE* f = fopen("profile.txt", "w");
    if (!f) return 0;
    fprintf(f, "Fat Princess Recomp profile. Exe base %p. Resolve exe+RVA with FatPrincessRecomp.map.\n",
            (void*)GetModuleHandleA(NULL));
    fflush(f);
    fprintf(stderr, "[profile] sampling every thread once a millisecond into profile.txt\n");
    g_samples.reserve(kMaxSamples);

    const DWORD self = GetCurrentThreadId();
    LARGE_INTEGER qf, q0, q;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&q0);
    long long last_refresh = 0;
    unsigned window_no = 0;
    for (;;) {
        if (timer) {
            LARGE_INTEGER due; due.QuadPart = -10000;   /* 1 ms */
            SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE);
            WaitForSingleObject(timer, 5);
        } else {
            Sleep(1);
        }
        QueryPerformanceCounter(&q);
        const long long ms = (q.QuadPart - q0.QuadPart) * 1000 / qf.QuadPart;
        /* Every 100 ms: SPU tasks run on short-lived threads of their own. */
        if (ms - last_refresh >= 100 || g_threads.empty()) { refresh_threads(self); last_refresh = ms; }

        for (size_t i = 0; i < g_threads.size(); i++) {
            ProfThread& t = g_threads[i];
            if (!t.alive) continue;
            ULONG64 c = 0;
            QueryThreadCycleTime(t.h, &c);
            if (c == t.cycles) continue;          /* idle since the last look */
            t.cycles = c;
            if (SuspendThread(t.h) == (DWORD)-1) continue;
            alignas(16) CONTEXT ctx;
            ctx.ContextFlags = CONTEXT_CONTROL;
            const BOOL got = GetThreadContext(t.h, &ctx);
            ResumeThread(t.h);
            if (got && g_samples.size() < kMaxSamples) {
                g_samples.push_back(Sample{ (uint32_t)i, (uint64_t)ctx.Rip });
                t.samples++;
            }
        }
        if (ms >= (long long)(window_no + 1) * 30000) {
            report(f, 30.0, ++window_no);
        }
    }
}

} // namespace

void fp_profile_start(void)
{
    const char* e = getenv("FP_PROFILE");
    if (!e || e[0] != '1') return;
    CloseHandle(CreateThread(NULL, 1u << 20, profiler_thread, NULL, 0, NULL));
}

#else
void fp_profile_start(void) {}
#endif
