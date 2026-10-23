// HWDER runtime core: code dispatch, logging/crash reporting, and process boot.
#include <exception>
#include <csignal>
#include <windows.h>
#include <psapi.h>
#include <windows.h>
#include <dbghelp.h>
#include <tlhelp32.h>
#include <timeapi.h>

#include <algorithm>
#include <atomic>
#include <mutex>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>
#include <unordered_map>
#include <thread>

#include "hwder/dispatch.h"
#include "hwder/module.h"
#include "hwder/runtime.h"
#include "launcher.h"
#include "kernel/kernel.h"
#include "service/ipc.h"
#include "gpu/display.h"
#include "fs/nsp.h"

// ------------------------------------------------------------------ dispatch
// One dense table per module text segment: (addr - text_lo) / 4 -> host function.
struct TextTable {
    u64 lo, hi;
    std::vector<HwFn> fns;
};
static std::vector<TextTable> g_tables;
extern "C" const unsigned long long hw_resumable[];
extern "C" const unsigned hw_resumable_count;
static std::vector<u64> g_starts;  // sorted function starts

static void build_dispatch() {
    for (unsigned m = 0; m < hw_module_count; m++) {
        TextTable t{hw_modules[m].text_lo, hw_modules[m].text_hi, {}};
        t.fns.assign((t.hi - t.lo) >> 2, nullptr);
        g_tables.push_back(std::move(t));
    }
    for (unsigned i = 0; i < hw_func_count; i++) {
        const HwFuncEntry& e = hw_func_table[i];
        for (auto& t : g_tables)
            if (e.addr >= t.lo && e.addr < t.hi) t.fns[(e.addr - t.lo) >> 2] = e.fn;
    }
    g_starts.reserve(hw_func_count);
    for (unsigned i = 0; i < hw_func_count; i++) g_starts.push_back(hw_func_table[i].addr);
    std::sort(g_starts.begin(), g_starts.end());
    hw_log("dispatch: %u functions in %u modules (%u resumable)", hw_func_count, hw_module_count, hw_resumable_count);
}

extern "C" HwFn hw_lookup(u64 addr) {
    for (auto& t : g_tables)
        if (addr >= t.lo && addr < t.hi) return t.fns[(addr - t.lo) >> 2];
    return nullptr;
}

// Paths are UTF-8 internally (the ANSI code page cannot spell every folder name a user may have).
static std::wstring to_w(const std::string& s) {
    int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    std::wstring w(n, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), w.data(), n);
    return w;
}
static std::string to_u8(const std::wstring& w) {
    int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string s(n, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), s.data(), n, nullptr, nullptr);
    return s;
}
static std::string env_u8(const char* name) {
    const wchar_t* e = _wgetenv(to_w(name).c_str());
    return e ? to_u8(e) : std::string();
}
static FILE* fopen_u8(const std::string& path, const char* mode) { return _wfopen(to_w(path).c_str(), to_w(mode).c_str()); }

static std::string exe_dir() {
    std::wstring p(32768, L'\0');
    DWORD n = GetModuleFileNameW(nullptr, p.data(), (DWORD)p.size());
    p.resize(n);
    std::string s = to_u8(p);
    return s.substr(0, s.find_last_of("\\/"));
}


// Functions with an unresolved indirect branch accept entry at any instruction (see gen.py):
// the dispatcher sets c->pc = target | 1 and the function's prologue jumps to that label.
static bool try_resume(Ctx* c) {
    u64 pc = c->pc;
    auto it = std::upper_bound(g_starts.begin(), g_starts.end(), pc);
    if (it == g_starts.begin()) return false;
    u64 start = *(it - 1);
    if (!std::binary_search(hw_resumable, hw_resumable + hw_resumable_count, start)) return false;
    HwFn f = hw_lookup(start);
    if (!f) return false;
    static std::atomic<int> logged{0};
    if (logged++ < 20) hw_log("dispatch: resuming 0x%llx inside f_%llx", (unsigned long long)pc, (unsigned long long)start);
    c->pc = pc | 1;
    f(c);
    return true;
}

static void unknown_target(Ctx* c) {
    if (try_resume(c)) return;
    hw_log("FATAL: jump to unrecompiled guest address 0x%llx (lr=0x%llx)", (unsigned long long)c->pc,
           (unsigned long long)c->x[30]);
    for (auto& t : g_tables) {
        if (c->pc >= t.lo && c->pc < t.hi) {
            // Record it so the next generator run adds it as an entry point.
            if (FILE* f = fopen_u8(exe_dir() + "\\extra_entries.txt", "a")) {
                fprintf(f, "0x%llx\n", (unsigned long long)c->pc);
                fclose(f);
            }
            hw_log("recorded 0x%llx in extra_entries.txt - regenerate to include it", (unsigned long long)c->pc);
        }
    }
    hw_fatal("unrecompiled target");
}

extern "C" void hw_call(Ctx* c, u64 addr) {
    HwFn f = hw_lookup(addr);
    if (!f) {
        c->pc = addr;
        unknown_target(c);
        return;
    }
    f(c);
}

extern "C" void hw_dispatch(Ctx* c) {
    HwFn f = hw_lookup(c->pc);
    if (!f) {
        unknown_target(c);
        return;
    }
    HW_MUSTTAIL return f(c);
}

// ------------------------------------------------------------------ misc runtime hooks
// Guest time scale (F2 turbo): the guest clock runs `scale` times faster than the host's and guest
// sleeps/timeouts shrink by the same factor, so the game's own frame pacing speeds up. The clock is
// re-anchored on every change so it stays continuous; the bases are immutable (old ones are leaked,
// a handful per session) so readers need no lock.
struct TimeBase {
    s64 real0;
    double virt0;
    double scale;
};
static std::atomic<const TimeBase*> g_time_base{new TimeBase{0, 0.0, 1.0}};
static double qpc_to_ticks() {
    static const double k = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return 19200000.0 / (double)f.QuadPart;
    }();
    return k;
}

extern "C" u64 hw_cntvct(void) {
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const TimeBase* b = g_time_base.load(std::memory_order_acquire);
    return (u64)(b->virt0 + (double)(t.QuadPart - b->real0) * qpc_to_ticks() * b->scale);
}

void hw_set_time_scale(double scale) {
    static std::mutex m;
    std::lock_guard<std::mutex> l(m);
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    const TimeBase* b = g_time_base.load(std::memory_order_acquire);
    double now = b->virt0 + (double)(t.QuadPart - b->real0) * qpc_to_ticks() * b->scale;
    g_time_base.store(new TimeBase{t.QuadPart, now, scale}, std::memory_order_release);
}

s64 hw_scaled_ns(s64 guest_ns) {
    double s = g_time_base.load(std::memory_order_acquire)->scale;
    if (guest_ns <= 0 || s == 1.0) return guest_ns;
    return std::max<s64>(1, (s64)(guest_ns / s));
}
extern "C" void hw_yield(void) { SwitchToThread(); }
extern "C" void hw_brk(Ctx* c, u64 pc, u32 imm) {
    hw_log("guest brk #0x%x at 0x%llx (x0=0x%llx)", imm, (unsigned long long)pc, (unsigned long long)c->x[0]);
    hw_fatal("guest abort");
}
extern "C" void hw_unimpl(Ctx* c, u64 pc, u32 word) {
    hw_log("unimplemented instruction %08x at 0x%llx", word, (unsigned long long)pc);
    hw_fatal("unimplemented instruction");
}
extern "C" void hw_sleep_ns(s64 ns) {
    if (ns <= 0) {
        SwitchToThread();
        return;
    }
    ns = hw_scaled_ns(ns);
    static thread_local HANDLE timer =
        CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    static const bool trace = getenv("HWDER_SLEEP_TRACE") != nullptr;
    static const s64 qpf = [] {
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        return f.QuadPart;
    }();
    if (ns > 5000000000ll) {  // long sleeps: precision is irrelevant (and ns * qpf would overflow)
        Sleep((DWORD)std::min<s64>(ns / 1000000, 0x7FFFFFFF));
        return;
    }
    LARGE_INTEGER t0;
    QueryPerformanceCounter(&t0);
    const s64 deadline = t0.QuadPart + ns * qpf / 1000000000;
    // The console wakes a sleeping thread within microseconds; a Windows timer can't wait less than
    // ~0.5 ms and often wakes later. The game polls with 1 us sleeps (each one cost ~0.7 ms here) and
    // paces its frames with ~14 ms sleeps (woken 0.4-0.7 ms late on average): sleep on the timer only up
    // to ~1 ms before the deadline, then yield-spin to it.
    constexpr s64 kSpinNs = 1000000;
    if (ns > kSpinNs + 500000 && timer) {
        LARGE_INTEGER due;
        due.QuadPart = -((ns - kSpinNs) / 100);
        if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0)) WaitForSingleObject(timer, INFINITE);
    }
    for (LARGE_INTEGER now;;) {
        QueryPerformanceCounter(&now);
        if (now.QuadPart >= deadline) break;
        SwitchToThread();
    }
    if (trace) {
        // HWDER_SLEEP_TRACE=1: per thread, every 10 s: how many sleeps, requested vs. actual duration.
        static thread_local u64 n = 0, req = 0, act = 0, worst = 0, last = GetTickCount64();
        static thread_local u64 buckets[6] = {};  // requested <100us <500us <1ms <4ms <17ms >=17ms
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        u64 us = (u64)((t1.QuadPart - t0.QuadPart) * 1000000 / f.QuadPart);
        n++;
        req += (u64)ns / 1000;
        act += us;
        worst = std::max(worst, us - std::min(us, (u64)ns / 1000));
        u64 r = (u64)ns / 1000;
        buckets[r < 100 ? 0 : r < 500 ? 1 : r < 1000 ? 2 : r < 4000 ? 3 : r < 17000 ? 4 : 5]++;
        if (GetTickCount64() - last > 10000) {
            hw_log("sleep: thread %lu: %llu sleeps, avg requested %llu us, avg actual %llu us, worst overshoot %llu us | <100us %llu <500us %llu <1ms %llu <4ms %llu <17ms %llu >=17ms %llu",
                   GetCurrentThreadId(), n, req / n, act / n, worst, buckets[0], buckets[1], buckets[2], buckets[3], buckets[4], buckets[5]);
            n = req = act = worst = 0;
            memset(buckets, 0, sizeof(buckets));
            last = GetTickCount64();
        }
    }
}

// ------------------------------------------------------------------ logging / crashes
static FILE* g_log;
static SRWLOCK g_log_lock = SRWLOCK_INIT;

extern "C" void hw_log(const char* fmt, ...) {
    char buf[4096];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    static const ULONGLONG t0 = GetTickCount64();
    unsigned ms = (unsigned)(GetTickCount64() - t0);
    AcquireSRWLockExclusive(&g_log_lock);
    fprintf(stderr, "[hwder] %s\n", buf);
    if (g_log) {
        fprintf(g_log, "[%3u.%03u] %s\n", ms / 1000, ms % 1000, buf);
        fflush(g_log);
    }
    ReleaseSRWLockExclusive(&g_log_lock);
}

extern "C" void hw_fatal(const char* why) {
    hw_log("fatal: %s", why);
    if (IsDebuggerPresent()) __debugbreak();
    if (getenv("HWDER_HOLD")) {  // keep the window up showing the reason
        display::set_status(std::string("halted: ") + why);
        for (;;) Sleep(1000);
    }
    ExitProcess(1);
}

static void report_crash(EXCEPTION_POINTERS* ep) {
    auto* er = ep->ExceptionRecord;
    HANDLE proc = GetCurrentProcess();
    // The symbol handler's module list dates from startup: pick up the DLLs loaded since (the Vulkan
    // loader, the GPU driver, layers) so their exports resolve.
    SymRefreshModuleList(proc);
    char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = (SYMBOL_INFO*)symbuf;
    // "module!symbol+0xN", or "module+0xRVA" when the module has no symbols (driver DLLs): the RVA is
    // what identifies the crash site in a bug report.
    auto describe = [&](u64 pc, char* out, size_t n) {
        HMODULE mod = nullptr;
        char mod_name[MAX_PATH] = "?";
        if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &mod))
            GetModuleBaseNameA(proc, mod, mod_name, sizeof(mod_name));
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        if (SymFromAddr(proc, pc, &disp, sym) && (!mod || sym->Address >= (u64)mod))
            snprintf(out, n, "%s!%s+0x%llx", mod_name, sym->Name, (unsigned long long)disp);
        else if (mod)
            snprintf(out, n, "%s+0x%llx", mod_name, (unsigned long long)(pc - (u64)mod));
        else
            snprintf(out, n, "?+0x%llx", (unsigned long long)pc);
    };
    char where[512];
    describe((u64)er->ExceptionAddress, where, sizeof(where));
    PWSTR tdesc = nullptr;
    GetThreadDescription(GetCurrentThread(), &tdesc);
    hw_log("CRASH code=0x%08lx at %p (%s) access=%s 0x%llx (thread %lu %ls)", er->ExceptionCode, er->ExceptionAddress, where,
           er->NumberParameters > 0 && er->ExceptionInformation[0] ? "write" : "read",
           er->NumberParameters > 1 ? (unsigned long long)er->ExceptionInformation[1] : 0ull, GetCurrentThreadId(), tdesc ? tdesc : L"");
    // Unwind with the OS's own unwind tables (present in every x64 module, symbols or not); StackWalk64
    // stops at the first module dbghelp does not know.
    CONTEXT ctx = *ep->ContextRecord;
    for (int i = 0; i < 40 && ctx.Rip; i++) {
        char fr[512];
        describe(ctx.Rip, fr, sizeof(fr));
        hw_log("  #%02d %s", i, fr);
        DWORD64 image_base = 0;
        PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx.Rip, &image_base, nullptr);
        if (fn) {
            void* handler_data = nullptr;
            DWORD64 establisher = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, image_base, ctx.Rip, fn, &ctx, &handler_data, &establisher, nullptr);
        } else {
            // Leaf function: the return address is at [rsp].
            if (!ctx.Rsp || IsBadReadPtr((const void*)ctx.Rsp, 8)) break;
            ctx.Rip = *(DWORD64*)ctx.Rsp;
            ctx.Rsp += 8;
        }
    }
    ExitProcess(2);
}

static bool is_fatal_code(DWORD code) {
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_ILLEGAL_INSTRUCTION || code == EXCEPTION_INT_DIVIDE_BY_ZERO ||
           code == EXCEPTION_STACK_OVERFLOW;
}

// First chance: faults in our own image (runtime + recompiled game code) are fatal right away. A fault
// inside another module (the Vulkan loader, the GPU driver, an overlay layer) can be one that module
// raises on purpose and catches with its own __try/__except - AMD's driver does this on the renderer
// thread - so it must reach the module's handler. If nothing handles it, unhandled_filter reports it.
static LONG WINAPI crash_handler(EXCEPTION_POINTERS* ep) {
    auto* er = ep->ExceptionRecord;
    if (!is_fatal_code(er->ExceptionCode)) return EXCEPTION_CONTINUE_SEARCH;
    if (er->ExceptionCode != EXCEPTION_STACK_OVERFLOW) {
        HMODULE mod = nullptr;
        GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           (LPCSTR)er->ExceptionAddress, &mod);
        if (mod && mod != GetModuleHandleW(nullptr)) {
            static std::atomic<int> logged{0};
            if (logged++ < 8) {
                char mn[MAX_PATH] = "?";
                GetModuleBaseNameA(GetCurrentProcess(), mod, mn, sizeof(mn));
                hw_log("first-chance exception 0x%08lx in %s+0x%llx (thread %lu), left to the module's handler", er->ExceptionCode,
                       mn, (unsigned long long)((u64)er->ExceptionAddress - (u64)mod), GetCurrentThreadId());
            }
            return EXCEPTION_CONTINUE_SEARCH;
        }
    }
    report_crash(ep);
    return EXCEPTION_CONTINUE_SEARCH;
}

static LONG WINAPI unhandled_filter(EXCEPTION_POINTERS* ep) {
    report_crash(ep);
    return EXCEPTION_EXECUTE_HANDLER;
}

// Log the calling thread's host stack (guest functions appear as f_<addr>).
extern "C" void hw_log_backtrace(const char* tag) {
    void* pcs[32];
    USHORT n = RtlCaptureStackBackTrace(1, 32, pcs, nullptr);
    HANDLE proc = GetCurrentProcess();
    char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
    auto* sym = (SYMBOL_INFO*)symbuf;
    std::string line;
    for (USHORT i = 0; i < n; i++) {
        sym->SizeOfStruct = sizeof(SYMBOL_INFO);
        sym->MaxNameLen = 255;
        DWORD64 disp = 0;
        const char* name = SymFromAddr(proc, (DWORD64)pcs[i], &disp, sym) ? sym->Name : "?";
        if (strncmp(name, "f_", 2) != 0) continue;  // guest frames only
        line += " ";
        line += name;
    }
    hw_log("%s backtrace:%s", tag, line.c_str());
}

// ------------------------------------------------------------------ thread dump (HWDER_THREAD_DUMP=<seconds>)
// Periodically logs every guest thread's host call stack (guest functions appear as f_<addr>).
static void dump_threads() {
    HANDLE proc = GetCurrentProcess();
    for (auto& t : kern::all_threads()) {
        if (!t->started || t->exited || !t->host_handle) continue;
        u64 pcs[24];
        int n = 0;
        if (SuspendThread(t->host_handle) == (DWORD)-1) continue;
        CONTEXT ctx = {};
        ctx.ContextFlags = CONTEXT_FULL;
        if (GetThreadContext(t->host_handle, &ctx)) {
            // RtlVirtualUnwind does not allocate, so it is safe while the target is suspended.
            for (; n < 24 && ctx.Rip; n++) {
                pcs[n] = ctx.Rip;
                DWORD64 base = 0;
                PRUNTIME_FUNCTION rf = RtlLookupFunctionEntry(ctx.Rip, &base, nullptr);
                if (!rf) {
                    ctx.Rip = *(DWORD64*)ctx.Rsp;
                    ctx.Rsp += 8;
                    continue;
                }
                void* hd = nullptr;
                DWORD64 ef = 0;
                RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx.Rip, rf, &ctx, &hd, &ef, nullptr);
            }
        }
        ResumeThread(t->host_handle);
        std::string line;
        char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = (SYMBOL_INFO*)symbuf;
        for (int i = 0; i < n; i++) {
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            char b[300];
            snprintf(b, sizeof(b), " %s+0x%llx", SymFromAddr(proc, pcs[i], &disp, sym) ? sym->Name : "?",
                     (unsigned long long)disp);
            line += b;
            if (i < n - 1) line += " <";
        }
        hw_log("thread %llu '%s' prio %d%s:%s", (unsigned long long)t->id, t->name.c_str(), t->priority,
               t->waiting ? " [waiting]" : "", line.c_str());
    }
}
static void start_thread_dumper() {
    const char* e = getenv("HWDER_THREAD_DUMP");
    if (!e || !atoi(e)) return;
    static int period = atoi(e);
    CreateThread(nullptr, 0, [](void*) -> DWORD {
        for (;;) {
            Sleep(period * 1000);
            hw_log("---- thread dump ----");
            dump_threads();
        }
    }, nullptr, 0, nullptr);
}

// Return addresses of a (possibly running) thread's stack from a register snapshot. The stack can
// change under us, so a bad read just ends the walk.
static int unwind_pcs(CONTEXT* ctx, u64* out, int max) {
    int n = 0;
    __try {
        while (n < max && ctx->Rip) {
            out[n++] = ctx->Rip;
            DWORD64 base = 0;
            PRUNTIME_FUNCTION fn = RtlLookupFunctionEntry(ctx->Rip, &base, nullptr);
            if (!fn) break;
            void* hd = nullptr;
            DWORD64 est = 0;
            RtlVirtualUnwind(UNW_FLAG_NHANDLER, base, ctx->Rip, fn, ctx, &hd, &est, nullptr);
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {
    }
    return n;
}

// HWDER_CPU_TRACE=1: every second, system-wide CPU use vs. this process's (is something else eating
// the CPU while the game stalls?).
static void start_cpu_trace() {
    if (!getenv("HWDER_CPU_TRACE")) return;
    std::thread([] {
        SetThreadDescription(GetCurrentThread(), L"HWDER cpu trace");
        auto ft = [](const FILETIME& f) { return ((u64)f.dwHighDateTime << 32) | f.dwLowDateTime; };
        FILETIME i0, k0, u0, c, e, pk0, pu0;
        GetSystemTimes(&i0, &k0, &u0);
        GetProcessTimes(GetCurrentProcess(), &c, &e, &pk0, &pu0);
        SYSTEM_INFO si;
        GetSystemInfo(&si);
        for (;;) {
            Sleep(1000);
            FILETIME i1, k1, u1, pk1, pu1;
            GetSystemTimes(&i1, &k1, &u1);
            GetProcessTimes(GetCurrentProcess(), &c, &e, &pk1, &pu1);
            double idle = (double)(ft(i1) - ft(i0)), total = (double)(ft(k1) - ft(k0) + ft(u1) - ft(u0));  // kernel includes idle
            double proc = (double)(ft(pk1) - ft(pk0) + ft(pu1) - ft(pu0));
            hw_log("cpu: system busy %.0f%%, this process %.0f%% (of %lu logical CPUs)", total > 0 ? 100.0 * (total - idle) / total : 0.0,
                   total > 0 ? 100.0 * proc / total : 0.0, si.dwNumberOfProcessors);
            i0 = i1, k0 = k1, u0 = u1, pk0 = pk1, pu0 = pu1;
        }
    }).detach();
}

// ------------------------------------------------------------------ sampling profiler
// HWDER_SAMPLE=<thread name substring> ("GPU", "guest", ...): samples every matching host thread ~1000x/s
// and logs per thread, every HWDER_SAMPLE_SECS (default 15) seconds, how busy it was and the functions
// it spent its time in - "self" (the sampled instruction) and "incl" (anywhere in the first 12 frames).
static void start_sampler() {
    static const char* want = getenv("HWDER_SAMPLE");
    if (!want || !*want) return;
    std::thread([] {
        SetThreadDescription(GetCurrentThread(), L"HWDER sampler");
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_TIME_CRITICAL);
        const char* secs_env = getenv("HWDER_SAMPLE_SECS");
        const u64 report_ms = (secs_env ? atoi(secs_env) : 15) * 1000ull;
        struct Target {
            HANDLE h;
            std::string name;
            std::unordered_map<u64, u32> self, incl;
            u32 samples = 0;
            u64 cycles0 = 0;
            u64 scan_cycles = 0;  // cycle count at the last scan
            bool active = true;   // used >5% of a core since the last scan: only these are sampled
        };
        std::vector<Target> targets;
        std::vector<DWORD> known;
        HANDLE proc = GetCurrentProcess();
        u64 last = GetTickCount64(), last_scan = 0;
        auto is_idle = [](const std::string& n) {
            return n.find("WaitFor") != std::string::npos || n.find("SleepConditionVariable") != std::string::npos ||
                   n.find("DelayExecution") != std::string::npos || n.find("RemoveIoCompletion") != std::string::npos;
        };
        auto report = [&] {
            SymRefreshModuleList(proc);
            char symbuf[sizeof(SYMBOL_INFO) + 256];
            auto name_of = [&](u64 pc) {
                auto* sym = (SYMBOL_INFO*)symbuf;
                memset(symbuf, 0, sizeof(symbuf));
                sym->SizeOfStruct = sizeof(SYMBOL_INFO);
                sym->MaxNameLen = 255;
                DWORD64 disp = 0;
                if (SymFromAddr(proc, pc, &disp, sym)) return std::string(sym->Name);
                HMODULE mod = nullptr;
                char mn[MAX_PATH] = "?";
                if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pc, &mod))
                    GetModuleBaseNameA(proc, mod, mn, sizeof(mn));
                return std::string(mn) + "!?";
            };
            auto fold = [&](std::unordered_map<u64, u32>& m) {
                std::unordered_map<std::string, u32> by;
                for (auto& [pc, n] : m) by[name_of(pc)] += n;
                std::vector<std::pair<u32, std::string>> v;
                for (auto& [k, n] : by) v.push_back({n, k});
                std::sort(v.begin(), v.end(), [](auto& a, auto& b) { return a.first > b.first; });
                return v;
            };
            for (Target& tg : targets) {
                if (!tg.samples) continue;  // inactive the whole window
                auto s = fold(tg.self);
                u32 idle = 0;
                for (auto& [n, k] : s)
                    if (is_idle(k)) idle += n;
                // CPU time the thread actually consumed over the window, independent of sampling.
                ULONG64 cyc = 0;
                QueryThreadCycleTime(tg.h, &cyc);
                hw_log("---- sampler: thread '%s': %u samples, %.0f%% busy, %.1f Gcycles ----", tg.name.c_str(), tg.samples,
                       100.0 * (tg.samples - idle) / tg.samples, (cyc - tg.cycles0) / 1e9);
                tg.cycles0 = cyc;
                if (tg.samples - idle >= tg.samples / 20) {  // mostly idle threads get no breakdown
                    for (size_t i = 0; i < s.size() && i < 20; i++)
                        hw_log("  self %5.1f%%  %s", 100.0 * s[i].first / tg.samples, s[i].second.c_str());
                    auto in = fold(tg.incl);
                    for (size_t i = 0, shown = 0; i < in.size() && shown < 25; i++) {
                        const std::string& k = in[i].second;
                        if (k.find("thread_start") != std::string::npos || k.find("BaseThreadInitThunk") != std::string::npos ||
                            k.find("RtlUserThreadStart") != std::string::npos || k.find("_Invoke") != std::string::npos)
                            continue;
                        hw_log("  incl %5.1f%%  %s", 100.0 * in[i].first / tg.samples, k.c_str());
                        shown++;
                    }
                }
                tg.self.clear();
                tg.incl.clear();
                tg.samples = 0;
            }
        };
        for (;;) {
            Sleep(1);
            u64 now = GetTickCount64();
            if (now - last_scan > 2000) {  // pick up new matching threads, re-rate the known ones
                // Suspending every matching thread each millisecond slows the game itself: only threads
                // that actually ran (>5% of a core over the last scan interval) are sampled.
                for (Target& tg : targets) {
                    ULONG64 cyc = 0;
                    QueryThreadCycleTime(tg.h, &cyc);
                    tg.active = (double)(cyc - tg.scan_cycles) > 0.05 * 3e9 * (now - last_scan) / 1000.0;
                    tg.scan_cycles = cyc;
                }
                last_scan = now;
                HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
                THREADENTRY32 te{sizeof(te)};
                for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te)) {
                    if (te.th32OwnerProcessID != GetCurrentProcessId()) continue;
                    if (std::find(known.begin(), known.end(), te.th32ThreadID) != known.end()) continue;
                    HANDLE h = OpenThread(THREAD_ALL_ACCESS, FALSE, te.th32ThreadID);
                    if (!h) continue;
                    PWSTR d = nullptr;
                    std::string n;
                    if (SUCCEEDED(GetThreadDescription(h, &d)) && d) {
                        char b[256];
                        WideCharToMultiByte(CP_UTF8, 0, d, -1, b, sizeof(b), nullptr, nullptr);
                        n = b;
                        LocalFree(d);
                    }
                    if (n.empty()) {
                        // Unnamed (service and loader threads): sampled under their id once they are
                        // old enough that a guest thread would have named itself by now.
                        static std::unordered_map<DWORD, int> seen;
                        if (++seen[te.th32ThreadID] < 2) {
                            CloseHandle(h);
                            continue;
                        }
                        n = "tid " + std::to_string(te.th32ThreadID);
                    }
                    known.push_back(te.th32ThreadID);
                    if ((strcmp(want, "*") != 0 && n.find(want) == std::string::npos) || n.find("sampler") != std::string::npos) {
                        CloseHandle(h);
                        continue;
                    }
                    Target tg;
                    tg.h = h;
                    tg.name = n;
                    ULONG64 cyc = 0;
                    QueryThreadCycleTime(h, &cyc);
                    tg.cycles0 = tg.scan_cycles = cyc;
                    targets.push_back(std::move(tg));
                    hw_log("sampler: sampling thread '%s'", n.c_str());
                }
                CloseHandle(snap);
            }
            for (Target& tg : targets) {
                if (!tg.active) continue;
                // Only the register snapshot happens while the thread is suspended: it may hold the heap or
                // loader lock, which anything else here (map inserts, unwind-table lookups) would need.
                if (SuspendThread(tg.h) == (DWORD)-1) continue;
                CONTEXT ctx{};
                ctx.ContextFlags = CONTEXT_FULL;
                bool got = GetThreadContext(tg.h, &ctx);
                ResumeThread(tg.h);
                if (!got) continue;
                // The stack is unwound after resuming: a racy read of a live stack only costs accuracy.
                tg.samples++;
                tg.self[ctx.Rip]++;
                u64 pcs[12];
                int n = unwind_pcs(&ctx, pcs, 12);
                for (int i = 0; i < n; i++) {
                    bool dup = false;
                    for (int k = 0; k < i; k++) dup |= pcs[k] == pcs[i];
                    if (!dup) tg.incl[pcs[i]]++;
                }
            }
            if (now - last >= report_ms) {
                report();
                last = now;
            }
        }
    }).detach();
}

// ------------------------------------------------------------------ NSO loading
static bool lz4_decompress(const u8* src, size_t src_size, u8* dst, size_t dst_size) {
    const u8* ip = src;
    const u8* iend = src + src_size;
    u8* op = dst;
    u8* oend = dst + dst_size;
    while (ip < iend) {
        u8 token = *ip++;
        size_t lit = token >> 4;
        if (lit == 15) {
            u8 b;
            do { b = *ip++; lit += b; } while (b == 255);
        }
        if (op + lit > oend || ip + lit > iend) return false;
        memcpy(op, ip, lit);
        op += lit;
        ip += lit;
        if (ip >= iend) break;
        size_t off = ip[0] | (ip[1] << 8);
        ip += 2;
        size_t len = token & 15;
        if (len == 15) {
            u8 b;
            do { b = *ip++; len += b; } while (b == 255);
        }
        len += 4;
        u8* match = op - off;
        if (match < dst || op + len > oend) return false;
        for (size_t i = 0; i < len; i++) op[i] = match[i];  // overlapping copy
        op += len;
    }
    return op == oend;
}

static std::vector<u8> read_file(const std::string& path) {
    std::vector<u8> d;
    FILE* f = fopen_u8(path, "rb");
    if (!f) return d;
    fseek(f, 0, SEEK_END);
    d.resize((size_t)ftell(f));
    fseek(f, 0, SEEK_SET);
    fread(d.data(), 1, d.size(), f);
    fclose(f);
    return d;
}

// ExeFS file: from the NSP when one is loaded, else from the extracted exefs directory.
static std::vector<u8> read_exefs(const std::string& exefs, const char* name) {
    std::vector<u8> d;
    if (hwfs::nsp::active()) {
        hwfs::nsp::read_exefs(name, d);
        return d;
    }
    return read_file(exefs + "\\" + name);
}

static void load_module(const HwModuleInfo& mi, const std::string& exefs) {
    std::vector<u8> nso = read_exefs(exefs, mi.name);
    if (nso.size() < 0x100 || memcmp(nso.data(), "NSO0", 4) != 0) {
        hw_log("loader: cannot read %s from %s", mi.name, hwfs::nsp::active() ? hwfs::nsp::path().c_str() : exefs.c_str());
        hw_fatal("missing ExeFS module");
    }
    u32 flags = *(u32*)&nso[0xC];
    struct Seg { u32 file_off, mem_off, size, extra; } seg[3];
    memcpy(seg, &nso[0x10], sizeof(seg));
    u32 csize[3];
    memcpy(csize, &nso[0x60], sizeof(csize));
    u64 bss = seg[2].extra;
    u64 image = (seg[2].mem_off + seg[2].size + bss + 0xFFF) & ~0xFFFull;
    kern::mem::map(mi.base, image, kern::mem::CodeData, kern::mem::RW);
    for (int i = 0; i < 3; i++) {
        u8* dst = (u8*)(mi.base + seg[i].mem_off);
        if (flags & (1u << i)) {
            if (!lz4_decompress(&nso[seg[i].file_off], csize[i], dst, seg[i].size)) hw_fatal("lz4");
        } else {
            memcpy(dst, &nso[seg[i].file_off], seg[i].size);
        }
    }
    // Experiment (HWDER_PATCH_DT=<hz>): the game's per-frame time step lives in two rodata floats
    // (1/60 s, plus 1/30 s for its half-rate mode). Rewriting them to 1/hz together with releasing
    // frames at hz tells us whether the simulation is dt-driven (then high refresh is feasible).
    if (!strcmp(mi.name, "main")) {
        if (const char* hz = getenv("HWDER_PATCH_DT")) {
            float f = (float)atof(hz);
            if (f >= 30.0f) {
                float dt = 1.0f / f, dt2 = 2.0f / f;
                memcpy((void*)(mi.base + 0x73c7d0), &dt, 4);
                memcpy((void*)(mi.base + 0x73f038), &dt, 4);
                memcpy((void*)(mi.base + 0x73c2ac), &dt2, 4);
                hw_log("loader: patched frame delta constants to 1/%.0f s", f);
            }
        }
    }
    u64 text_end = (seg[0].mem_off + seg[0].size + 0xFFF) & ~0xFFFull;
    u64 ro_end = (seg[1].mem_off + seg[1].size + 0xFFF) & ~0xFFFull;
    kern::mem::map(mi.base, text_end, kern::mem::Code, kern::mem::RX, false);
    kern::mem::map(mi.base + seg[1].mem_off, ro_end - seg[1].mem_off, kern::mem::Code, kern::mem::R, false);
    kern::mem::map(mi.base + seg[2].mem_off, image - seg[2].mem_off, kern::mem::CodeData, kern::mem::RW, false);
    hw_log("loader: %-8s at 0x%llx (0x%llx bytes)", mi.name, (unsigned long long)mi.base, (unsigned long long)image);
}

static void load_npdm(const std::string& exefs) {
    std::vector<u8> d = read_exefs(exefs, "main.npdm");
    if (d.size() < 0x80 || memcmp(d.data(), "META", 4) != 0) return;
    kern::g_process.main_thread_priority = d[0xE];
    kern::g_process.main_thread_core = d[0xF];
    kern::g_process.main_thread_stack = *(u32*)&d[0x1C];
    hw_log("npdm: main thread prio %u core %u stack 0x%x", kern::g_process.main_thread_priority,
           kern::g_process.main_thread_core, kern::g_process.main_thread_stack);
}

// ------------------------------------------------------------------ boot
void hw_config_load(const std::string& dir);

int main(int argc, char** argv) {
    std::string dir = exe_dir();
    SetCurrentDirectoryW(to_w(dir).c_str());  // caches, settings and saves live beside the exe
    std::string log_path = env_u8("HWDER_LOG");
    g_log = fopen_u8(log_path.empty() ? dir + "\\hwder.log" : log_path, "w");
    SymSetOptions(SYMOPT_UNDNAME | SYMOPT_DEFERRED_LOADS);
    SymInitialize(GetCurrentProcess(), nullptr, TRUE);
    AddVectoredExceptionHandler(1, crash_handler);
    SetUnhandledExceptionFilter(unhandled_filter);
    // Deaths that bypass SEH: C++ terminate (uncaught exception, e.g. in a worker thread), abort(),
    // CRT invalid-parameter. Log them with the thread id instead of dying silently.
    std::set_terminate([] {
        hw_log("TERMINATE: std::terminate called (uncaught exception?) on thread %lu", GetCurrentThreadId());
        ExitProcess(3);
    });
    signal(SIGABRT, [](int) {
        PWSTR desc = nullptr;
        GetThreadDescription(GetCurrentThread(), &desc);
        hw_log("ABORT: abort() called on thread %lu (%ls)", GetCurrentThreadId(), desc ? desc : L"?");
        void* pcs[48];
        USHORT n = RtlCaptureStackBackTrace(0, 48, pcs, nullptr);
        char symbuf[sizeof(SYMBOL_INFO) + 256] = {};
        auto* sym = (SYMBOL_INFO*)symbuf;
        for (USHORT i = 0; i < n; i++) {
            sym->SizeOfStruct = sizeof(SYMBOL_INFO);
            sym->MaxNameLen = 255;
            DWORD64 disp = 0;
            const char* name = SymFromAddr(GetCurrentProcess(), (DWORD64)pcs[i], &disp, sym) ? sym->Name : "?";
            HMODULE mod = nullptr;
            char modname[MAX_PATH] = "?";
            if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)pcs[i], &mod))
                GetModuleBaseNameA(GetCurrentProcess(), mod, modname, sizeof(modname));
            hw_log("  #%02u %s!%s+0x%llx", i, modname, name, (unsigned long long)disp);
        }
        ExitProcess(3);
    });
    _set_abort_behavior(0, _WRITE_ABORT_MSG | _CALL_REPORTFAULT);
    _set_invalid_parameter_handler([](const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
        hw_log("CRT: invalid parameter on thread %lu", GetCurrentThreadId());
    });
    timeBeginPeriod(1);
    hw_log("HWDER - Hyrule Warriors: Definitive Edition static recompilation");
    hw_config_load(dir);
    if (!launcher::run(dir)) return 0;  // the user closed the launcher

    std::string exefs = env_u8("HWDER_EXEFS");
    if (exefs.empty()) exefs = dir + "\\exefs";
    {
        // Game data: an NSP beside the exe (decrypted in-process with the user's prod.keys) or an
        // extracted exefs/ + romfs/ pair. Nothing is written out either way.
        std::string err;
        bool nsp = hwfs::nsp::init(dir, err);
        bool have_dir = GetFileAttributesW(to_w(exefs + "\\main").c_str()) != INVALID_FILE_ATTRIBUTES;
        if (!nsp && !have_dir) {
            std::string msg = err.empty() ? "No game data found.\n\nPut your own dump of Hyrule Warriors: Definitive Edition "
                                            "(the .nsp) and your prod.keys next to hwder.exe.\n\n(Looked for *.nsp in: " + dir + ")"
                                          : "Cannot load the game from the NSP:\n\n" + err;
            hw_log("boot: %s", msg.c_str());
            if (!getenv("HWDER_MUTE")) MessageBoxW(nullptr, to_w(msg).c_str(), L"HWDER", MB_ICONERROR | MB_OK);
            return 2;
        }
        hw_log("boot: game data from %s", nsp ? hwfs::nsp::path().c_str() : exefs.c_str());
    }
    kern::mem::init();
    build_dispatch();
    display::init();
    ipc::register_all_services();
    load_npdm(exefs);
    for (unsigned i = 0; i < hw_module_count; i++) load_module(hw_modules[i], exefs);

    // Main thread: stack in the stack region, entry at rtld (the first module), x1 = own handle.
    u64 stack_size = (kern::g_process.main_thread_stack + 0xFFF) & ~0xFFFull;
    u64 stack_base = kern::mem::kStackBase;
    kern::mem::map(stack_base, stack_size, kern::mem::Stack, kern::mem::RW);
    auto t = kern::create_thread(hw_modules[0].text_lo, 0, stack_base + stack_size,
                                 (s32)kern::g_process.main_thread_priority, (s32)kern::g_process.main_thread_core);
    t->handle = kern::g_handles.add(t);
    t->ctx->x[1] = t->handle;
    t->name = "MainThread";
    hw_log("boot: main thread entry 0x%llx, stack 0x%llx+0x%llx", (unsigned long long)t->entry,
           (unsigned long long)stack_base, (unsigned long long)stack_size);
    kern::start_thread(t.get());
    start_thread_dumper();
    start_sampler();
    start_cpu_trace();
    WaitForSingleObject(t->host_handle, INFINITE);
    hw_log("main thread exited");
    return 0;
}
