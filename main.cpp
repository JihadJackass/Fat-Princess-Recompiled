/*
 * ps3recomp game project -- entry point template.
 *
 * This is the runner. It boots a lifted title through the toolkit's PPU
 * scaffold: allocate the flat guest VM, load the PPU ELF named on the command
 * line, register the lifted function table and the HLE NID handlers, start a
 * frame clock, and dispatch the entry OPD.
 *
 * The scaffold does the work; nothing below reimplements it. ppu_load_elf,
 * ppu_recomp_register, ppu_hle_init, ppu_sysprx_register, ppu_fs_register,
 * lv2_init_syscalls and ppu_run all come from the toolkit, compiled into this
 * project from PS3RECOMP_DIR (see CMakeLists.txt). What is left here is the
 * part a port owns: which backend to present through, how the frame clock is
 * paced, and whatever diagnostics the title turns out to need.
 *
 * runtime/ppu/tests/boot_main.cpp in the toolkit is the same boot in its
 * fully-instrumented form -- crash filter, hang watchdog, guest-PC sampling
 * profiler. Read it when a boot goes wrong; copy from it what the title
 * needs. It is deliberately not what a fresh project starts with.
 *
 * The one part that IS duplicated from it is the frame clock below, because a
 * port is expected to change its pacing and a shared one would be the wrong
 * shape for that. It was copied faithfully, comments and all. If a pacing bug
 * is fixed in one of the two, look at the other.
 *
 * This builds on Windows, macOS and Linux. CI builds and runs it against the
 * boot smoke title on the last two, and compile-checks it against clang-cl on
 * the first (tools/check_ppu_scaffold.py).
 */

/* The lifter's generated header, which declares ppu_context and the function
 * table. It comes first: everything below is written against that struct. */
#include "ppu_recomp.h"

/* The Win32 names -- Sleep, GetTickCount64, CreateThread, InterlockedIncrement,
 * VirtualAlloc, the scalar typedefs -- from one place. On Windows this is a
 * passthrough to <windows.h>; off Windows the toolkit's shim supplies the same
 * names over pthreads and mmap, so the call sites below are written once. */
#include "win32_compat.h"
#include "ps3emu/ps3_ftrace.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <math.h>
#include <vector>

#ifndef _WIN32
#include <sys/mman.h>   /* mmap: the guest arena, see alloc_guest_vm */
#endif

#if defined(__APPLE__)
#include <CoreFoundation/CoreFoundation.h>   /* the main thread's run loop, see run_guest */
#include <pthread.h>
#endif

#ifdef _WIN32
/* timeBeginPeriod: <windows.h> arrives with WIN32_LEAN_AND_MEAN set, which
 * excludes the multimedia timer API, so it is asked for by name -- and after
 * windows.h, since timeapi.h uses UINT. No shim exists off Windows because
 * there is nothing to shim: the 15.6 ms default timer granularity this works
 * around is a Windows problem. */
#include <timeapi.h>
#endif

/* ---------------------------------------------------------------------------
 * What the scaffold gives this file
 * -----------------------------------------------------------------------*/
extern "C" {
uint32_t ppu_load_elf(const char* path);      /* ELF -> guest RAM, returns entry OPD */
void     ppu_recomp_register(void);           /* generated: lifted table -> address map */
void     ppu_hle_init(void);                  /* firmware import NID -> HLE handler */
void     ppu_sysprx_register(void);           /* boot-critical CRT (sys_initialize_tls, ...) */
void     ppu_fs_register(void);               /* cellFs over the real game directory */
void     lv2_init_syscalls(void);             /* the lv2 syscall table */
int      ppu_run(uint32_t entry_opd, uint32_t stack_top);

/* This project's own: load whatever real system PRX modules the title needs
 * into guest RAM and register their exports. stubs.cpp has the empty version
 * a game with no lifted PRX wants. */
void     ps3_load_prx_modules(void);

extern const char* ppu_vfs_root;              /* host dir the PS3 mount points map into */
void     ps3_host_quit(const char* why);      /* cellSysutil.c: REQUEST_EXITGAME, then exit 0 */

/* sys_ppu_thread.c: the main thread's stack, as sys_ppu_thread_get_stack_information
 * reports it. Left at 0 it names VM_STACK_BASE, the first created thread's stack. */
extern uint32_t g_ppu_main_stack_addr;
extern uint32_t g_ppu_main_stack_size;

/* The status the guest handed to sys_process_exit. A title that exits that way
 * never returns through ppu_run, so this is only read on the path where the
 * entry function unwound instead. */
extern int     g_sys_process_exit_called;
extern int32_t g_sys_process_exit_code;

/* Host-provided symbols the runtime and the HLE libraries link against. */
uint8_t* vm_base = nullptr;
extern uint32_t ppu_vm_size;                  /* ppu_loader.cpp: the OOB guard */

/* Guest-callback dispatch. g_ps3_guest_caller is the hook the HLE runtime
 * calls back into recompiled code through -- cellSysutil events and the GCM
 * vblank/flip handlers. ppu_guest_call does the OPD -> dispatch. */
typedef void (*ps3_guest_caller_fn)(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                                    uint64_t, uint64_t, uint64_t, uint64_t);
extern ps3_guest_caller_fn g_ps3_guest_caller;
uint64_t ppu_guest_call(uint32_t, uint64_t, uint64_t, uint64_t, uint64_t,
                        uint64_t, uint64_t, uint64_t, uint64_t);

/* The GCM half of the frame clock. */
void     cellGcmTickVBlank(void);
int      cellGcm_fps_unlocked(void);   /* FrameLimit=unlocked */
void     cellGcm_unlocked_beat(void);
void     cellGcmTickFlip(void);
int      cellGcm_take_flip_pending(void);
void     cellGcm_rsx_process_fifo(void);      /* drain get -> put */
unsigned cellGcm_flip_request_count(void);
}

/* ---------------------------------------------------------------------------
 * RSX present backend
 * -----------------------------------------------------------------------*/
/* D3D12 on Windows, Metal on Apple, and the null backend's headless software
 * path anywhere else -- the same selection runtime/host/host_posix.c and the
 * toolkit's boot harness make. All three expose the same three entry points,
 * so the frame clock below is backend-agnostic. */
#if defined(_WIN32) && defined(PS3RECOMP_RSX_VULKAN)
/* Windows with the Vulkan renderer built in (the default): Vulkan, unless
 * PS3_RENDERER=d3d12 asks for the D3D12 backend. Picked once, before init. */
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
extern "C" int  rsx_vulkan_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_vulkan_backend_present(void);
extern "C" int  rsx_vulkan_backend_pump_messages(void);
static int fp_use_d3d12(void)
{
    static int v = -1;
    if (v < 0) { const char* e = getenv("PS3_RENDERER"); v = (e && !_stricmp(e, "d3d12")) ? 1 : 0; }
    return v;
}
static int  rsx_backend_init(uint32_t w, uint32_t h, const char* t)
{ return fp_use_d3d12() ? rsx_d3d12_backend_init(w, h, t) : rsx_vulkan_backend_init(w, h, t); }
static void rsx_backend_present(void)
{ if (fp_use_d3d12()) rsx_d3d12_backend_present(); else rsx_vulkan_backend_present(); }
static int  rsx_backend_pump(void)
{ return fp_use_d3d12() ? rsx_d3d12_backend_pump_messages() : rsx_vulkan_backend_pump_messages(); }
extern "C" void rsx_vulkan_backend_set_title(const char* title);
/* The D3D12 backend writes its own FPS into its title. */
static void rsx_backend_set_title(const char* t) { if (!fp_use_d3d12()) rsx_vulkan_backend_set_title(t); }
extern "C" void rsx_vulkan_backend_perf(unsigned long long* waits, double* wait_ms);
static void rsx_backend_perf(unsigned long long* w, double* ms) { if (!fp_use_d3d12()) rsx_vulkan_backend_perf(w, ms); }
#  define RSX_BACKEND_NAME    (fp_use_d3d12() ? "D3D12" : "Vulkan")
#elif defined(_WIN32)
extern "C" int  rsx_d3d12_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_d3d12_backend_present(void);
extern "C" int  rsx_d3d12_backend_pump_messages(void);
#  define rsx_backend_init    rsx_d3d12_backend_init
#  define rsx_backend_present rsx_d3d12_backend_present
#  define rsx_backend_pump    rsx_d3d12_backend_pump_messages
#  define RSX_BACKEND_NAME    "D3D12"
static void rsx_backend_set_title(const char*) {}
static void rsx_backend_perf(unsigned long long*, double*) {}
#elif defined(__APPLE__)
extern "C" int  rsx_metal_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_metal_backend_present(void);
extern "C" int  rsx_metal_backend_pump_messages(void);
#  define rsx_backend_init    rsx_metal_backend_init
#  define rsx_backend_present rsx_metal_backend_present
#  define rsx_backend_pump    rsx_metal_backend_pump_messages
#  define RSX_BACKEND_NAME    "Metal"
static void rsx_backend_set_title(const char*) {}
static void rsx_backend_perf(unsigned long long*, double*) {}
#elif defined(PS3RECOMP_RSX_VULKAN)
/* Linux test harness only (the shipped game is Windows/D3D12). */
extern "C" int  rsx_vulkan_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_vulkan_backend_present(void);
extern "C" int  rsx_vulkan_backend_pump_messages(void);
#  define rsx_backend_init    rsx_vulkan_backend_init
#  define rsx_backend_present rsx_vulkan_backend_present
#  define rsx_backend_pump    rsx_vulkan_backend_pump_messages
#  define RSX_BACKEND_NAME    "Vulkan"
extern "C" void rsx_vulkan_backend_set_title(const char* title);
static void rsx_backend_set_title(const char* t) { rsx_vulkan_backend_set_title(t); }
extern "C" void rsx_vulkan_backend_perf(unsigned long long* waits, double* wait_ms);
static void rsx_backend_perf(unsigned long long* w, double* ms) { rsx_vulkan_backend_perf(w, ms); }
#else
extern "C" int  rsx_null_backend_init(uint32_t w, uint32_t h, const char* title);
extern "C" void rsx_null_backend_present(void);
extern "C" int  rsx_null_backend_pump_messages(void);
#  define rsx_backend_init    rsx_null_backend_init
#  define rsx_backend_present rsx_null_backend_present
#  define rsx_backend_pump    rsx_null_backend_pump_messages
#  define RSX_BACKEND_NAME    "null (headless software)"
static void rsx_backend_set_title(const char*) {}
static void rsx_backend_perf(unsigned long long*, double*) {}
#endif

/* ---------------------------------------------------------------------------
 * Configuration
 * -----------------------------------------------------------------------*/
/* The flat VM must span every region the PS3 memory map uses, not just the
 * game image: sys_ppu_thread_create puts thread stacks at 0xD0000000, so a
 * smaller arena makes every spawned thread's stack access out of bounds. */
#define VM_SIZE       0x100010000ull   /* the full 32-bit guest space + a 64K guard */
#define STACK_TOP     0x0FF00000u      /* main-thread stack, below the 0x10000000 segment */
#define STACK_SIZE    0x00100000u      /* what the main thread is told it has */
#define WINDOW_WIDTH  1280
#define WINDOW_HEIGHT 720

/* ---------------------------------------------------------------------------
 * Frame clock
 * -----------------------------------------------------------------------*/
/* On real hardware the RSX raises a vblank interrupt about 60 times a second
 * and that is what drives the game's frame loop. With no RSX it is synthesized
 * here: a host thread calls cellGcmTickVBlank/TickFlip, which invoke the
 * handlers the guest registered, and drains the GCM FIFO. Without this the
 * title initialises, registers its handlers and then waits forever.
 *
 * The ticks are driven off real elapsed time rather than off how long present()
 * took. A hidden or occluded window makes present block hard, and pacing the
 * ticks behind it paces the whole game behind it. */
static volatile LONG g_frames_presented = 0;

/* Frames handed to the backend at a guest FLIP boundary -- one per frame the
 * guest actually finished. The presents made before the guest's first flip, so
 * a fresh window is not left blank through a long boot, carry no guest frame
 * and are deliberately not counted. */
extern "C" unsigned ppu_boot_frames_presented(void)
{
    return (unsigned)g_frames_presented;
}

/* Frame-clock thread time split, for the [perf] line: how long presents and
 * FIFO drains take. That thread does both, and the game's main loop waits for
 * its flips, so these bound the frame rate. */
#include <chrono>
static double g_perf_present_ms = 0.0, g_perf_fifo_ms = 0.0;
static unsigned g_perf_presents = 0;
extern "C" void perf_report_frame(void);                   /* perf_report.cpp */
extern "C" void perf_report_host(double drain_ms, double present_ms);
static inline double perf_now_ms(void)
{
    return std::chrono::duration<double, std::milli>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}
static void timed_present(void)
{
    const double t0 = perf_now_ms();
    rsx_backend_present();
    const double dt = perf_now_ms() - t0;
    g_perf_present_ms += dt;
    perf_report_host(0.0, dt);
    g_perf_presents++;
}
static void timed_fifo(void)
{
    const double t0 = perf_now_ms();
    cellGcm_rsx_process_fifo();
    const double dt = perf_now_ms() - t0;
    g_perf_fifo_ms += dt;
    perf_report_host(dt, 0.0);
}

/* ---------------------------------------------------------------------------
 * Frame-clock timing
 *
 * The frame clock used to Sleep(4) between passes, and the game cannot start
 * its next frame until this thread has drained its commands and presented, so
 * that sleep sat inside every frame. Now a 1 ms wait on a high-resolution
 * waitable timer (precise and focus-independent, Windows 10 1803+), and the
 * vblank beat on the performance counter at the PS3's 59.94 Hz rather than
 * whole-millisecond 16 ms steps (62.5 Hz).
 * -----------------------------------------------------------------------*/
static HANDLE   s_hrtimer;
static long long s_qpf;

static long long qpc_now(void)
{
    LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart;
}

/* Block for about `us` microseconds. */
static void clock_wait_us(long long us)
{
    if (us <= 0) return;
#ifdef _WIN32
    if (s_hrtimer) {
        LARGE_INTEGER due; due.QuadPart = -(us * 10);   /* 100 ns units, relative */
        if (SetWaitableTimer(s_hrtimer, &due, 0, NULL, NULL, FALSE)) {
            WaitForSingleObject(s_hrtimer, (DWORD)(us / 1000 + 2));
            return;
        }
    }
#endif
    Sleep((DWORD)((us + 999) / 1000));
}

/* The vblank beat on its own thread. It used to be ticked from the frame
 * clock's loop, which also drains the FIFO (several ms a frame) and holds
 * presents for vsync -- so vblanks went out late and then in bursts. Fat
 * Princess paces its frames off its vblank handler, so that showed up as
 * frames released early after late ones: the game lurching between sluggish
 * and too fast. On the console the vblank is an interrupt on the beat.
 * FP_VBLANK_THREAD=0 puts the ticks back in the frame clock loop. */
static volatile LONG s_vblank_thread_on = 0;
static long long s_vblank_base, s_vblank_period;
static DWORD WINAPI vblank_clock(LPVOID)
{
#ifdef _WIN32
    HANDLE timer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* HIGH_RESOLUTION */, TIMER_ALL_ACCESS);
    SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
    SetThreadDescription(GetCurrentThread(), L"vblank clock");
#else
    void* timer = NULL;
#endif
    long long next = s_vblank_base;
    for (;;) {
        for (;;) {   /* sleep most of the way on the timer, yield the rest */
            const long long left = next - qpc_now();
            if (left <= 0) break;
            /* Unlocked frame rate: answer the game's refresh wait every
             * millisecond between the real refreshes too. */
            if (cellGcm_fps_unlocked()) {
                cellGcm_unlocked_beat();
#ifdef _WIN32
                if (timer && left * 1000000 / s_qpf > 1200) {
                    LARGE_INTEGER due; due.QuadPart = -10000;   /* 1 ms */
                    if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE)) { WaitForSingleObject(timer, 3); continue; }
                }
#endif
                Sleep(left * 1000000 / s_qpf > 1200 ? 1 : 0);
                continue;
            }
            const long long us = left * 1000000 / s_qpf;
#ifdef _WIN32
            if (us > 1500 && timer) {
                LARGE_INTEGER due; due.QuadPart = -((us - 1000) * 10);
                if (SetWaitableTimer(timer, &due, 0, NULL, NULL, FALSE))
                    WaitForSingleObject(timer, (DWORD)(us / 1000 + 2));
                else Sleep(1);
            } else
#endif
            if (us > 1500) { (void)timer; Sleep(1); }
            else Sleep(0);
        }
        const long long now = qpc_now();
        int n = 0;
        while (now - next >= 0 && n < 4) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            next += s_vblank_period;
            n++;
        }
        if (now - next >= 0)   /* stalled for many beats (debugger, sleep): resync, do not replay */
            next = now + s_vblank_period - (now - s_vblank_base) % s_vblank_period;
    }
    return 0;
}

/* Block until the performance counter reaches `due`. */
static void wait_until_qpc(long long due)
{
    for (;;) {
        const long long left = due - qpc_now();
        if (left <= 0) return;
        const long long us = left * 1000000 / s_qpf;
        if (us > 1500) clock_wait_us(us - 1000);   /* sleep most of it ... */
        else if (us > 200) Sleep(0);               /* ... then yield up to the deadline */
    }
}

/* Vsync flips. The game asks for CELL_GCM_DISPLAY_VSYNC (cellGcmSetFlipMode
 * 1): on the console a flip lands on a vertical blank, so it shows at most one
 * new frame per 59.94 Hz refresh -- and Fat Princess steps its game logic once
 * per frame. The flip mode was recorded but never honoured, so a light scene
 * flipped at 120 fps and the whole game ran double speed. A flip now waits for
 * the next vblank slot when one has already flipped in the current one.
 * [Graphics] FrameLimit=unlocked (FP_FRAME_PACING=unlocked) turns this off. */
extern "C" uint32_t cellGcm_flip_mode(void);

static void vsync_flip_wait(void)
{
    static int unlocked = -1;
    static long long last_slot = -1;
    if (unlocked < 0) { const char* e = getenv("FP_FRAME_PACING");
                        unlocked = (e && !strcmp(e, "unlocked")) ? 1 : 0;
                        if (unlocked) fprintf(stderr, "[frame] vsync pacing off: the game runs at any frame rate\n"); }
    if (unlocked || !s_vblank_period || cellGcm_flip_mode() != 1 /* CELL_GCM_DISPLAY_VSYNC */) return;
    long long slot = (qpc_now() - s_vblank_base) / s_vblank_period;
    if (slot <= last_slot) {
        slot = last_slot + 1;
        wait_until_qpc(s_vblank_base + slot * s_vblank_period);
    }
    last_slot = slot;
}

/* An extra cap below the refresh rate: [Graphics] FrameLimit=30 (FP_FRAME_LIMIT).
 * The slot advances by one period per frame (no drift) and resyncs after a
 * stall instead of rushing to catch up. */
static void frame_limit_wait(void)
{
    static int fps = -1;
    static long long next_due = 0;
    if (fps < 0) { const char* e = getenv("FP_FRAME_LIMIT"); fps = e ? atoi(e) : 0;
                   if (fps < 0 || fps > 1000) fps = 0;
                   if (fps) fprintf(stderr, "[frame] limit %d fps\n", fps); }
    if (!fps || !s_qpf) return;
    /* 30 = every second refresh of the console's 59.94 Hz beat (29.97 FPS),
     * so the cap and the game's own two-refresh wait agree and never drift. */
    const long long period = (fps == 30 && s_vblank_period) ? 2 * s_vblank_period : s_qpf / fps;
    const long long now = qpc_now();
    if (!next_due || now - next_due > period * 4) next_due = now;   /* first frame / long stall */
    wait_until_qpc(next_due);
    next_due += period;
}

/* F9 in the game window: record the next 10 seconds of the frame timeline
 * (PS3_FTRACE: syscalls, sleeps, SPU waits, RSX drains, flips, presents) into
 * ftrace.txt -- which thread waits for which, frame by frame. fp_config arms
 * the recorder with a far-off start when the profile is on; F9 moves the start
 * to now. One recording per run. */
extern "C" void ps3_hang_dump(const char* why);   /* runtime/ppu/ppu_loader.cpp */
extern "C" void rsx_draw_engine_perf_line(char* buf, size_t n);
extern "C" void ps3_hle_write_stats(void);        /* runtime/ppu/ppu_hle.cpp */
extern "C" void perf_report_second(char* out, size_t n);   /* perf_report.cpp */
extern "C" void fp_data_patches_install(void);           /* fp_data_patches.cpp */
extern "C" void perf_report_request(int seconds);

static void ftrace_hotkey(void)
{
#ifdef _WIN32
    static ULONGLONG last = 0;
    static bool was_down = false;
    const ULONGLONG now = GetTickCount64();
    if (now - last < 30) return;
    last = now;
    const bool down = (GetAsyncKeyState(VK_F9) & 0x8000) != 0;
    const bool pressed = down && !was_down;
    was_down = down;
    /* F10: write the freeze report (what every thread is waiting on) to the
     * log now -- for a stall the automatic one below does not catch. */
    { static bool f10_was = false;
      const bool f10 = (GetAsyncKeyState(VK_F10) & 0x8000) != 0;
      if (f10 && !f10_was) {
          DWORD p10 = 0;
          GetWindowThreadProcessId(GetForegroundWindow(), &p10);
          if (p10 == GetCurrentProcessId()) ps3_hang_dump("F10 pressed");
      }
      f10_was = f10; }
    /* F8: record 30 s and write perf_report.txt (perf_report.cpp). */
    { static bool f8_was = false;
      const bool f8 = (GetAsyncKeyState(VK_F8) & 0x8000) != 0;
      if (f8 && !f8_was) {
          DWORD p8 = 0;
          GetWindowThreadProcessId(GetForegroundWindow(), &p8);
          if (p8 == GetCurrentProcessId()) perf_report_request(30);
      }
      f8_was = f8; }
    /* Unattended runs (bench.bat): FP_EXIT_FILE and FP_TRACE_FILE name files
     * that a driver outside the process creates -- quit cleanly, or record a
     * 10 s timeline as F9 would. Each is deleted when seen. */
    { static int init = 0; static char exitf[512], tracef[512];
      static ULONGLONG last_poll = 0;
      if (!init) {
          init = 1;
          const char* e = getenv("FP_EXIT_FILE");  if (e) snprintf(exitf, sizeof exitf, "%s", e);
          const char* t = getenv("FP_TRACE_FILE"); if (t) snprintf(tracef, sizeof tracef, "%s", t);
      }
      if ((exitf[0] || tracef[0]) && now - last_poll >= 250) {
          last_poll = now;
          if (exitf[0] && GetFileAttributesA(exitf) != INVALID_FILE_ATTRIBUTES) {
              DeleteFileA(exitf);
              fprintf(stderr, "[bench] quit file seen, exiting\n");
              fflush(stderr); fflush(stdout);
              ExitProcess(0);
          }
          if (tracef[0] && GetFileAttributesA(tracef) != INVALID_FILE_ATTRIBUTES) {
              DeleteFileA(tracef);
              if (PS3_FT_ON() && !g_ps3_ft.dumped) {
                  const uint64_t t = ps3_ft_now();
                  g_ps3_ft.end = t + 10ull * 1000000000ull;
                  g_ps3_ft.start = t;
                  fprintf(stderr, "[ftrace] trace file: recording the next 10 s into %s\n", g_ps3_ft.path);
              } else {
                  fprintf(stderr, "[ftrace] trace file ignored (recorder off or already used)\n");
              }
          }
      } }
    if (!pressed) return;
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    if (pid != GetCurrentProcessId()) return;
    if (!PS3_FT_ON()) {
        fprintf(stderr, "[ftrace] F9: the timeline recorder is off (turn on the profile in the launcher)\n");
        return;
    }
    ps3_ft_state* s = &g_ps3_ft;
    if (s->dumped) { fprintf(stderr, "[ftrace] F9: already recorded this run (ftrace.txt)\n"); return; }
    const uint64_t t = ps3_ft_now();
    s->end = t + 10ull * 1000000000ull;
    s->start = t;
    fprintf(stderr, "[ftrace] F9: recording the next 10 s into %s\n", s->path);
#endif
}

/* FP_MEMDIFF=<seconds>: a development probe. That many seconds after the first
 * frame, copy the game's data and bss (0xC40000..0xFDEC48) at three consecutive
 * frames and log every word that moved by the SAME amount both times: the
 * per-frame counters and fixed time steps the game advances, which a 60 FPS
 * mode has to halve. FP_MEMDIFF_LO / FP_MEMDIFF_HI pick another range. */
static void memdiff_probe(void)
{
    static int state = -1; static double t_start = 0; static uint32_t lo, hi;
    static std::vector<uint8_t> snap[3];
    if (state == -1) {
        const char* e = getenv("FP_MEMDIFF");
        if (!e) { state = 99; return; }
        t_start = perf_now_ms() + atof(e) * 1000.0; state = 0;
        lo = getenv("FP_MEMDIFF_LO") ? (uint32_t)strtoul(getenv("FP_MEMDIFF_LO"), 0, 16) : 0xC40000u;
        hi = getenv("FP_MEMDIFF_HI") ? (uint32_t)strtoul(getenv("FP_MEMDIFF_HI"), 0, 16) : 0xFDEC48u;
    }
    if (state >= 3 || perf_now_ms() < t_start) return;
    snap[state].assign(vm_base + lo, vm_base + hi);
    if (++state < 3) return;
    auto w = [&](int s, uint32_t o) { const uint8_t* q = &snap[s][o];
        return (uint32_t)q[0] << 24 | (uint32_t)q[1] << 16 | (uint32_t)q[2] << 8 | q[3]; };
    int shown = 0;
    for (uint32_t o = 0; o + 4 <= hi - lo && shown < 400; o += 4) {
        const uint32_t a = w(0, o), b = w(1, o), c = w(2, o);
        if (a == b || b == c) continue;
        float fa, fb, fc; memcpy(&fa, &a, 4); memcpy(&fb, &b, 4); memcpy(&fc, &c, 4);
        const float d1 = fb - fa, d2 = fc - fb;
        const bool fstep = std::isfinite(fa) && std::isfinite(fc) && d1 != 0 && fabsf(d1 - d2) <= fabsf(d1) * 1e-3f &&
                           fabsf(fa) < 1e7f && fabsf(d1) > 1e-6f;
        const bool istep = (int32_t)(b - a) == (int32_t)(c - b) && (int32_t)(b - a) != 0 && (int32_t)(b - a) > -100000 && (int32_t)(b - a) < 100000;
        if (!fstep && !istep) continue;
        fprintf(stderr, "[memdiff] 0x%08X  %s step %g  (%g %g %g | 0x%08X 0x%08X 0x%08X)\n", lo + o,
                fstep ? "float" : "int", fstep ? (double)d1 : (double)(int32_t)(b - a),
                (double)fa, (double)fb, (double)fc, a, b, c);
        shown++;
    }
    /* Where a 30 FPS frame's step is kept: words holding 1/30 (or 2, 30, 1/60)
     * that did not change, so a 60 FPS mode can find what to halve. */
    for (uint32_t o = 0; o + 4 <= hi - lo; o += 4) {
        const uint32_t a = w(0, o);
        if (a != w(2, o)) continue;
        const char* what = a == 0x3D088889u ? "1/30" : a == 0x3C888889u ? "1/60" : a == 0x41F00000u ? "30.0" :
                           (a >= 0x3D088880u && a <= 0x3D088890u) ? "~1/30" : NULL;
        if (what) fprintf(stderr, "[memdiff] const %s at 0x%08X\n", what, lo + o);
    }
    fprintf(stderr, "[memdiff] done, %d candidates\n", shown);
}

static void present_guest_frame(void)
{
    memdiff_probe();
    vsync_flip_wait();
    frame_limit_wait();
    timed_present();
    /* This thread increments and guest threads read, so interlocked rather
     * than a volatile ++, which on arm64 is neither atomic nor a fence. */
    InterlockedIncrement(&g_frames_presented);
    perf_report_frame();
}

/* FPS counter: once a second, the guest frames finished in that second and
 * the average since the first one, in the window title (FP_SHOW_FPS=0 hides
 * it) and every 5 s in the log as "[perf]". Frames are guest flips, so this is
 * the game's own frame rate, not the window's refresh. */
static void fps_tick(const char* title)
{
    static ULONGLONG t_last = 0, t_first = 0;
    static LONG f_last = 0, f_first = 0;
    static int show = -1, n = 0;
    static double best_avg = 0.0;
    if (show < 0) { const char* e = getenv("FP_SHOW_FPS"); show = (e && e[0] == '0') ? 0 : 1; }
    const ULONGLONG now = GetTickCount64();
    const LONG f = g_frames_presented;
    /* Freeze report: once the game has run a while, 6 s with no guest frame
     * writes what every guest thread and SPU task is waiting on to the log
     * (once per stall). Loading screens still flip, so this is a real hang. */
    { static LONG f_seen = -1; static ULONGLONG t_seen = 0; static bool reported = false;
      if (f != f_seen) { f_seen = f; t_seen = now; reported = false; }
      else if (!reported && f > 300 && now - t_seen >= 6000) {
          reported = true;
          char why[96];
          snprintf(why, sizeof why, "no frame for 6 s after frame %ld", (long)f);
          ps3_hang_dump(why);
      } }
    if (!t_last) { t_last = now; f_last = f; return; }
    if (now - t_last < 1000) return;
    if (!t_first && f > 0) { t_first = t_last; f_first = f_last; }
    const double fps = (double)(f - f_last) * 1000.0 / (double)(now - t_last);
    const double avg = t_first && now > t_first
                     ? (double)(f - f_first) * 1000.0 / (double)(now - t_first) : 0.0;
    (void)best_avg;
    t_last = now; f_last = f;
    /* Performance measurement (perf_report.cpp): the busiest guest thread's
     * split of a frame, the PC's CPU load, and F8's 30 s report. */
    char extra[256];
    perf_report_second(extra, sizeof extra);
    if (show) {
        char buf[512];
        snprintf(buf, sizeof buf, "%s  |  %.0f FPS  (avg %.1f)%s%s", title, fps, avg,
                 extra[0] ? "  |  " : "", extra);
        rsx_backend_set_title(buf);
    }
    if ((++n % 5) == 0) {
        /* per 5 s: present = ms per present, fifo = ms of FIFO draining per second */
        unsigned long long gw = 0; double gms = 0.0;
        rsx_backend_perf(&gw, &gms);
        /* gpu = GPU round trips (submit + wait) per frame and ms per second
         * spent blocked in them: high gpu-wait means the GPU or the sync, low
         * means the CPU-side work in fifo= is the cost. */
        fprintf(stderr, "[perf] t=%llus fps=%.1f avg=%.1f frames=%ld present=%.2fms x%u fifo=%.0fms/s "
                "gpu=%.1f/frame %.0fms/s\n",
                (unsigned long long)(t_first ? (now - t_first) / 1000 : 0), fps, avg, (long)f,
                g_perf_presents ? g_perf_present_ms / g_perf_presents : 0.0, g_perf_presents,
                g_perf_fifo_ms / 5.0, g_perf_presents ? (double)gw / g_perf_presents : 0.0, gms / 5.0);
        g_perf_present_ms = g_perf_fifo_ms = 0.0; g_perf_presents = 0;
        /* Where the drain's draw-engine time went (rsx_draw_engine.c). */
        char eb[512];
        rsx_draw_engine_perf_line(eb, sizeof eb);
        fprintf(stderr, "[perf-rsx] per 5 s: %s\n", eb);
        /* PS3_HLE_STATS: refresh the import-coverage file every 30 s. */
        { static int tick = 0; if (++tick % 6 == 0) ps3_hle_write_stats(); }
    }
}

static DWORD WINAPI frame_clock(LPVOID)
{
    const char* title = getenv("PS3_TITLE");
    if (!title || !*title) title = "ps3recomp";

    int rsx_ok = (rsx_backend_init(WINDOW_WIDTH, WINDOW_HEIGHT, title) == 0);
    fprintf(stderr, "[rsx] %s backend init %s\n", RSX_BACKEND_NAME,
            rsx_ok ? "OK -- window open" : "FAILED");

    unsigned  last_flip = 0;
#ifdef _WIN32
    s_hrtimer = CreateWaitableTimerExW(NULL, NULL, 0x00000002 /* CREATE_WAITABLE_TIMER_HIGH_RESOLUTION */,
                                       TIMER_ALL_ACCESS);
#endif
    { LARGE_INTEGER f; QueryPerformanceFrequency(&f); s_qpf = f.QuadPart; }
    const long long vblank_period = s_qpf * 1001 / 60000;   /* 59.94 Hz */
    long long next_tick = qpc_now();
    s_vblank_base = next_tick;
    s_vblank_period = vblank_period;
    { const char* e = getenv("FP_VBLANK_THREAD");
      if (!(e && e[0] == '0') && CreateThread(NULL, 256u * 1024, vblank_clock, NULL, 0, NULL)) {
          s_vblank_thread_on = 1;
          fprintf(stderr, "[frame] vblank beat on its own thread\n");
      } }

    for (;;) {
        clock_wait_us(1000);
        const long long now = qpc_now();

        int fired = 0;
        if (s_vblank_thread_on) next_tick = now + vblank_period;   /* the vblank thread ticks */
        while (!s_vblank_thread_on && now - next_tick >= 0 && fired < 240) {
            cellGcmTickVBlank();
            cellGcmTickFlip();
            /* Present a pending flip BEFORE draining any further. The flip
             * fires at a get==put frame boundary on the guest thread, so the
             * batch held right now is exactly the completed frame; presenting
             * after the drain races the guest's next-frame writes and shows a
             * mixed one. */
            if (rsx_ok && cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            /* Drain the FIFO every tick. This is what writes the RSX sync-fence
             * labels the game's per-frame logic blocks on, so it has to keep
             * advancing at 60 Hz even while present() throttles. */
            if (rsx_ok) timed_fifo();
            next_tick += vblank_period;
            fired++;
        }
        if (fired >= 240) next_tick = now;   /* fell too far behind -- resync */

        /* Drain at the outer cadence too, not only on the 16 ms tick: titles
         * fence every render pass on a label the drain writes, and one wave of
         * those at 16 ms apiece paces the guest into single-figure frame rates.
         * The real RSX writes them in microseconds. */
        if (rsx_ok) {
            if (cellGcm_take_flip_pending()) {
                present_guest_frame();
                last_flip = cellGcm_flip_request_count();
            }
            timed_fifo();

            fps_tick(title);
            ftrace_hotkey();
            if (rsx_backend_pump() != 0) {
                rsx_ok = 0;              /* window closed */
                /* ...so quit, as the console's Quit Game does, instead of
                 * leaving the game running with no window. */
                ps3_host_quit("window closed");
                continue;
            }
            /* Present on a guest flip. A present on a fixed clock can catch the
             * drain mid-frame and flash a partial one. Before the first flip
             * present freely, so the window is not blank during boot. */
            unsigned fc = cellGcm_flip_request_count();
            if (fc != last_flip) {
                /* This present covers the flip the drain just reached, so take
                 * its pending flag too. Left set, the next pass presented the
                 * same frame a second time: two presents per game frame, the
                 * second one waiting a whole extra refresh in vsync_flip_wait
                 * with the drain stalled behind it, and the FPS counter counting
                 * both (so it read double the real frame rate). */
                (void)cellGcm_take_flip_pending();
                present_guest_frame();
                last_flip = fc;
            } else if (fc == 0) {
                timed_present();
            }
        }
    }
    return 0;
}

/* ---------------------------------------------------------------------------
 * VFS root
 * -----------------------------------------------------------------------*/
/* Derive the directory holding PS3_GAME from the EBOOT path
 * <root>/PS3_GAME/USRDIR/EBOOT.elf -> <root>. $PS3_VFS_ROOT overrides. */
static char s_vfs_root[1024];

static void derive_vfs_root(const char* eboot)
{
    const char* env = getenv("PS3_VFS_ROOT");
    if (env && *env) { ppu_vfs_root = env; return; }

    strncpy(s_vfs_root, eboot, sizeof s_vfs_root - 1);
    for (char* p = s_vfs_root; *p; p++) if (*p == '\\') *p = '/';
    /* strip EBOOT.elf, USRDIR and PS3_GAME */
    for (int i = 0; i < 3; i++) { char* s = strrchr(s_vfs_root, '/'); if (s) *s = 0; }
    if (!s_vfs_root[0]) strcpy(s_vfs_root, ".");
    ppu_vfs_root = s_vfs_root;
}

/* ---------------------------------------------------------------------------
 * Guest VM
 * -----------------------------------------------------------------------*/
#ifdef _WIN32
/* Demand-paging for the flat VM: reserve the whole 4 GB guest space, which
 * costs no commit, and commit each 64 KB page on first access. Every 32-bit
 * guest offset is then valid, so a garbage guest pointer reads zero instead of
 * killing the process. Faults outside the arena fall through untouched. */
static LONG WINAPI vm_commit_veh(EXCEPTION_POINTERS* ep)
{
    if (ep->ExceptionRecord->ExceptionCode == EXCEPTION_ACCESS_VIOLATION) {
        ULONG_PTR fault = ep->ExceptionRecord->ExceptionInformation[1];
        uintptr_t base  = (uintptr_t)vm_base;
        if (vm_base && fault >= base && fault < base + VM_SIZE) {
            void* page = (void*)(fault & ~(uintptr_t)0xFFFF);
            if (VirtualAlloc(page, 0x10000, MEM_COMMIT, PAGE_READWRITE))
                return EXCEPTION_CONTINUE_EXECUTION;
        }
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
#endif

static bool alloc_guest_vm(void)
{
#ifdef _WIN32
    AddVectoredExceptionHandler(1, vm_commit_veh);
    vm_base = (uint8_t*)VirtualAlloc(NULL, VM_SIZE, MEM_RESERVE, PAGE_READWRITE);
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#else
    /* No vectored exception handlers off Windows, so the arena is mapped up
     * front and lazily backed by the OS: only pages the title touches cost
     * anything. It spans the whole 32-bit space plus the guard, as on Windows.
     * It used to stop below 0xE0000000, but the raw SPU windows (local store
     * and problem-state registers, runtime/spu/spu_raw.c) live from there up.
     * Every load and store a title made into one was reported as an
     * out-of-bounds access and dropped, so a raw SPU was never loaded or
     * started, and sys_raw_spu_create, which clears the registers through
     * vm_base directly, wrote past the end of the mapping. With the whole
     * space backed, ppu_vm_size 0 also gives the loader's bounds check the
     * one-compare fast path Windows already has.
     *
     * mmap, not calloc: vm_commit() hands vm_base + addr to mprotect, which
     * takes page-aligned addresses only. glibc serves an allocation this big
     * from its own mmap chunk and returns a pointer 16 bytes past the page
     * start, so every commit failed with EINVAL and sys_memory_allocate
     * answered ENOMEM to every call. mmap's base is page-aligned and zeroed. */
    void* p = mmap(NULL, VM_SIZE, PROT_READ | PROT_WRITE,
                   MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    vm_base = (p == MAP_FAILED) ? nullptr : (uint8_t*)p;
    ppu_vm_size = 0;              /* the whole space is backed; no OOB guard needed */
#endif
    return vm_base != nullptr;
}

/* ---------------------------------------------------------------------------
 * Main
 * -----------------------------------------------------------------------*/
static void harness_guest_caller(uint32_t opd, uint64_t a0, uint64_t a1,
                                 uint64_t a2, uint64_t a3, uint64_t a4,
                                 uint64_t a5, uint64_t a6, uint64_t a7)
{
    ppu_guest_call(opd, a0, a1, a2, a3, a4, a5, a6, a7);
}

/* ---------------------------------------------------------------------------
 * Running the guest
 * -----------------------------------------------------------------------*/
#if defined(__APPLE__)
/* AppKit only runs on the process's main thread. The Metal backend creates
 * its window and pumps its events with dispatch_sync onto the main queue
 * when it is called from any other thread, as it is from the frame clock.
 * With the guest on the main thread nothing ever serviced that queue: the
 * frame clock blocked in its first rsx_backend_init, no vblank or flip was
 * ever delivered and the FIFO never drained. A title that waits for a flip
 * hangs there. So the guest runs on a thread of its own, with the same
 * 256 MB host stack every guest thread gets (sys_ppu_thread.c), and the
 * main thread turns the run loop until ppu_run returns. Headless runs never
 * touch the main queue, which is why CI does not see the difference. */
struct guest_run { uint32_t entry, stack_top; int rc; volatile int done; };

static void* guest_thread(void* arg)
{
    guest_run* g = (guest_run*)arg;
    g->rc = ppu_run(g->entry, g->stack_top);
    __atomic_store_n(&g->done, 1, __ATOMIC_RELEASE);
    CFRunLoopStop(CFRunLoopGetMain());
    return nullptr;
}

static int run_guest(uint32_t entry, uint32_t stack_top)
{
    static guest_run g;
    g.entry = entry; g.stack_top = stack_top; g.rc = 0; g.done = 0;
    pthread_attr_t attr;
    pthread_attr_init(&attr);
    pthread_attr_setstacksize(&attr, 256u * 1024u * 1024u);
    pthread_t th;
    int err = pthread_create(&th, &attr, guest_thread, &g);
    pthread_attr_destroy(&attr);
    if (err != 0) {
        fprintf(stderr, "[boot] could not start the guest thread (%d)\n", err);
        return -1;
    }
    /* Short slices: CFRunLoopStop only ends a run that is already going. */
    while (!__atomic_load_n(&g.done, __ATOMIC_ACQUIRE))
        CFRunLoopRunInMode(kCFRunLoopDefaultMode, 0.05, false);
    pthread_join(th, nullptr);
    return g.rc;
}
#else
static int run_guest(uint32_t entry, uint32_t stack_top) { return ppu_run(entry, stack_top); }
#endif

extern "C" void fp_config_load(void);   /* fp_config.cpp: fatprincess.ini */
void fp_profile_start(void);            /* fp_profile.cpp: FP_PROFILE=1 */
extern "C" const char* fp_config_default_elf(void);

int main(int argc, char** argv)
{
    fp_config_load();   /* before anything reads its switches */
    fp_data_patches_install();   /* optional data patches (fp_data_patches.cpp) */
    fp_profile_start();
#if defined(_WIN32) && defined(FP_SPU_AVX2)
    /* The lifted SPU code is built for AVX2 (CMake FP_SPU_AVX2). On a CPU
     * without it the first SPU float instruction would crash with no word of
     * why; say so instead. PF_AVX2_INSTRUCTIONS_AVAILABLE = 40 (Windows 10+). */
    if (!IsProcessorFeaturePresent(40)) {
        MessageBoxA(NULL, "This build of Fat Princess needs a processor with AVX2 "
                          "(Intel Core 4th generation or newer, or AMD Ryzen).\n\n"
                          "Rebuild with -DFP_SPU_AVX2=OFF for older processors.",
                    "Fat Princess", MB_ICONERROR);
        return 1;
    }
#endif
    /* No argument: the game folder's GAME.elf (launcher / double-click). */
    static char* fp_argv[3];
    if (argc < 2) {
        fp_argv[0] = argv[0];
        fp_argv[1] = (char*)fp_config_default_elf();
        fp_argv[2] = nullptr;
        argv = fp_argv; argc = 2;
    }

#ifdef _WIN32
#pragma comment(lib, "winmm.lib")
    /* 1 ms timer resolution. The default granularity is about 15.6 ms, which
     * inflates every shorter wait the title makes and throttles the whole
     * thing. POSIX timers are already fine-grained. */
    timeBeginPeriod(1);
    setvbuf(stdout, NULL, _IONBF, 0);   /* unbuffered: do not lose prints on a kill */
#endif

    printf("=== ps3recomp game runner ===\n");

    if (!alloc_guest_vm()) {
        fprintf(stderr, "ERROR: could not allocate the guest address space\n");
        return 1;
    }

    uint32_t entry = ppu_load_elf(argv[1]);
    if (!entry) {
        fprintf(stderr, "ERROR: could not load %s\n", argv[1]);
        return 1;
    }

    derive_vfs_root(argv[1]);
    printf("[boot] VFS root: %s\n", ppu_vfs_root);

    ppu_recomp_register();   /* lifted function table -> address map */
    ps3_load_prx_modules();  /* this game's lifted system PRX, if it has any */
    ppu_hle_init();          /* firmware import NID -> HLE handlers */
    ppu_sysprx_register();   /* boot-critical CRT */
    ppu_fs_register();       /* cellFs over the game directory */
    lv2_init_syscalls();     /* the lv2 syscall table */

    /* Install the guest-callback hook, then start the frame clock. It no-ops
     * until the title registers its vblank and flip handlers during init. */
    g_ps3_guest_caller = harness_guest_caller;
    CreateThread(NULL, 4u * 1024 * 1024, frame_clock, NULL, 0, NULL);

    printf("\n[boot] dispatching entry OPD 0x%08X (stack top 0x%08X)\n\n",
           entry, STACK_TOP);

    /* A title that reads its own stack bounds (to find data its parent left
     * below the top, say) gets the stack it really runs on. */
    g_ppu_main_stack_addr = STACK_TOP - STACK_SIZE;
    g_ppu_main_stack_size = STACK_SIZE;

    int rc = run_guest(entry, STACK_TOP);
    printf("\n[boot] ppu_run returned %d (entry function unwound)\n", rc);

    /* A title that called sys_process_exit never reaches this line: that path
     * ends in the host exit(). Getting here means the entry function returned
     * instead, so hand back the status the guest published if it published
     * one. Returning a hardcoded 0 would report success for a run that never
     * got anywhere. */
    return g_sys_process_exit_called ? (int)g_sys_process_exit_code : 0;
}
