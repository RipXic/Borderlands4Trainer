// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// Borderlands 4 Trainer - core: config, log (rotation, crash logs, session markers), memory tools, address expressions,
// main-thread runner, crash diagnostics, guarded calls, DLL entry.
#define _CRT_RAND_S
#include "common.h"
#include <tlhelp32.h>
#include <psapi.h>
#include <cstdarg>
#include <deque>
#include <fstream>
#include <share.h>
#include <algorithm>
#include "MinHook.h"
#include "host_api.h"
#include <dbghelp.h>
#include <filesystem>
#pragma comment(lib, "dbghelp.lib")
#include <thread>
#include <csignal>
#include <exception>

namespace wb {

Config g_cfg;
std::atomic<bool> g_retired{false};
std::atomic<bool> g_game_exiting{false};
std::atomic<bool> g_features_off{false};
std::atomic<bool> g_reload_failed{false};
static std::atomic<int> g_drain_active{0};
static HANDLE g_active_marker = nullptr;
static HMODULE g_self = nullptr;

// ------------------------------------------------------------------ util
std::string hexstr(uint64_t v) { char b[32]; sprintf_s(b, "0x%llX", (unsigned long long)v); return b; }

// reply envelope {ok, code, message, data, warnings} (was in server.cpp; the PUBLIC build has no server but the panel uses it)
json ok(json data, const std::string& msg) {
    return { {"ok", true}, {"code", "OK"}, {"message", msg}, {"data", data}, {"warnings", json::array()} };
}
json fail(const std::string& code, const std::string& msg) {
    // NOT_SOLO is shown to players: say what it means and what to send if it is wrong (the report shows why)
    const std::string m = code != "NOT_SOLO" ? msg
        : "Not in someone else's game: you joined another player's game, so their game decides. Not true? Please send the trainer report (Settings > Support).";
    return { {"ok", false}, {"code", code}, {"message", m}, {"data", nullptr}, {"warnings", json::array()} };
}

std::string to_hex(const uint8_t* p, size_t n) {
    std::string s; char b[4];
    for (size_t i = 0; i < n; ++i) { sprintf_s(b, "%02X", p[i]); if (i) s += ' '; s += b; }
    return s;
}

// "48 89 05 ?? ?? ?? ??" (single ? also accepted)
bool parse_pattern(const std::string& s, std::vector<int>& pat) {
    size_t i = 0;
    while (i < s.size()) {
        if (isspace((unsigned char)s[i])) { ++i; continue; }
        if (s[i] == '?') { pat.push_back(-1); while (i < s.size() && s[i] == '?') ++i; continue; }
        if (i + 1 >= s.size() || !isxdigit((unsigned char)s[i]) || !isxdigit((unsigned char)s[i + 1])) return false;
        pat.push_back((int)strtoul(s.substr(i, 2).c_str(), nullptr, 16)); i += 2;
    }
    return !pat.empty();
}

// ------------------------------------------------------------------ log
static std::mutex g_log_mtx;
static std::deque<std::string> g_log_ring;
static FILE* g_log_file = nullptr;

static thread_local bool t_in_log = false;
void log(const char* fmt, ...) {
    if (t_in_log) return;
    char msg[2048];
    va_list ap; va_start(ap, fmt); vsnprintf(msg, sizeof msg, fmt, ap); va_end(ap);
    SYSTEMTIME t; GetLocalTime(&t);
    char line[2200];
    sprintf_s(line, "[%02d:%02d:%02d.%03d] %s", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, msg);
    t_in_log = true;
    struct Reset { ~Reset() { t_in_log = false; } } reset;
    std::lock_guard<std::mutex> lk(g_log_mtx);
    g_log_ring.push_back(line);
    while (g_log_ring.size() > 800) g_log_ring.pop_front();
    if (g_log_file) { fprintf(g_log_file, "%s\n", line); fflush(g_log_file); }
}

HMODULE self_module() { return g_self; }
// read by the loader: 1 started, 2 hooks installed, 3 hooks failed, 4 panel draws, 5 folder name not usable, 6 panel setup failed
void set_attach_state(long v) {
    (void)v;   // 1.1.0: the trainer is DLL-only (dsound.dll loads it) - there is no loader window to report to
}
void set_attach_state_min(long v) {   // the panel may already draw by the time the install call returns
    static std::mutex m; std::lock_guard<std::mutex> lk(m);
    if (!g_overlay_drawn) set_attach_state(v);
}
std::atomic<bool> g_overlay_drawn{ false };
void release_active_marker() { if (g_active_marker) { CloseHandle(g_active_marker); g_active_marker = nullptr; } }
void log_close() { std::lock_guard<std::mutex> lk(g_log_mtx); if (g_log_file) { fclose(g_log_file); g_log_file = nullptr; } }

std::vector<std::string> log_tail(size_t n) {
    std::lock_guard<std::mutex> lk(g_log_mtx);
    size_t start = g_log_ring.size() > n ? g_log_ring.size() - n : 0;
    return std::vector<std::string>(g_log_ring.begin() + start, g_log_ring.end());
}

// ------------------------------------------------------------------ config
void load_config() {
    std::ifstream f(g_cfg.root + "\\config.json");
    if (!f) return;   // optional in the PUBLIC build (only lock_input + the panel path overrides matter); never written
    try {
        json j = json::parse(f);
        g_cfg.port = j.value("port", g_cfg.port);
        g_cfg.allow_raw = j.value("allow_raw", g_cfg.allow_raw);
        g_cfg.lock_input = j.value("lock_input", g_cfg.lock_input);
        g_cfg.backup_minutes = j.value("backup_minutes", g_cfg.backup_minutes);
        g_cfg.tx_cooldown_ms = j.value("tx_cooldown_ms", g_cfg.tx_cooldown_ms);
    } catch (std::exception& e) {
        log("config.json is not valid JSON (%s) - using defaults.", e.what());
    }
}

void save_config() {
    // (the panel hotkey moved to settings.json - see settings.cpp)
    json j = { {"port", g_cfg.port}, {"allow_raw", g_cfg.allow_raw},
               {"lock_input", g_cfg.lock_input}, {"backup_minutes", g_cfg.backup_minutes}, {"tx_cooldown_ms", g_cfg.tx_cooldown_ms} };
    std::ofstream f(g_cfg.root + "\\config.json");
    f << j.dump(2);
}

// ------------------------------------------------------------------ modules / memory
static std::mutex g_mod_mtx;
static std::vector<ModuleInfo> g_mods;

std::vector<ModuleInfo> list_modules(bool refresh) {
    std::lock_guard<std::mutex> lk(g_mod_mtx);
    if (refresh || g_mods.empty()) {
        g_mods.clear();
        HMODULE mods[2048]; DWORD needed = 0;
        if (EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) {
            for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 2048; ++i) {
                MODULEINFO mi{}; char name[MAX_PATH]{};
                GetModuleInformation(GetCurrentProcess(), mods[i], &mi, sizeof mi);
                GetModuleBaseNameA(GetCurrentProcess(), mods[i], name, MAX_PATH);
                g_mods.push_back({ name, (uintptr_t)mi.lpBaseOfDll, (size_t)mi.SizeOfImage });
            }
        }
    }
    return g_mods;
}

std::string module_name_of(uintptr_t a) {
    for (auto& m : list_modules()) if (a >= m.base && a < m.base + m.size) return m.name;
    return "";
}

uintptr_t module_base(const std::string& name, size_t* size) {
    for (int pass = 0; pass < 2; ++pass)
        for (auto& m : list_modules(pass == 1))
            if (_stricmp(m.name.c_str(), name.c_str()) == 0) { if (size) *size = m.size; return m.base; }
    return 0;
}

uintptr_t game_base(size_t* size) {
    static uintptr_t b = 0; static size_t s = 0;
    if (!b) b = module_base(kGameExe, &s);
    if (size) *size = s;
    return b;
}

bool safe_read(uintptr_t addr, void* out, size_t n) {
    if (addr < 0x10000) return false;
    SIZE_T got = 0;
    return ReadProcessMemory(GetCurrentProcess(), (LPCVOID)addr, out, n, &got) && got == n;
}

bool safe_write(uintptr_t addr, const void* in, size_t n) {
    if (addr < 0x10000) return false;
    DWORD old = 0;
    if (!VirtualProtect((LPVOID)addr, n, PAGE_EXECUTE_READWRITE, &old)) return false;
    SIZE_T put = 0;
    BOOL okw = WriteProcessMemory(GetCurrentProcess(), (LPVOID)addr, in, n, &put);
    DWORD tmp; VirtualProtect((LPVOID)addr, n, old, &tmp);
    FlushInstructionCache(GetCurrentProcess(), (LPCVOID)addr, n);
    return okw && put == n;
}

std::vector<Region> readable_regions(uintptr_t base, size_t size) {
    std::vector<Region> out;
    uintptr_t a = base, end = base + size;
    while (a < end) {
        MEMORY_BASIC_INFORMATION mbi{};
        if (!VirtualQuery((LPCVOID)a, &mbi, sizeof mbi)) break;
        uintptr_t rs = (uintptr_t)mbi.BaseAddress, re = std::min<uintptr_t>(rs + mbi.RegionSize, end);
        DWORD p = mbi.Protect & 0xFF;
        bool readable = mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && p != PAGE_NOACCESS;
        bool exec = p == PAGE_EXECUTE_READ || p == PAGE_EXECUTE_READWRITE || p == PAGE_EXECUTE_WRITECOPY || p == PAGE_EXECUTE;
        if (readable) out.push_back({ std::max(rs, a), (size_t)(re - std::max(rs, a)), exec });
        a = re;
    }
    return out;
}

// ------------------------------------------------------------------ address expressions
namespace {
struct Parser {
    const std::string& s; size_t i = 0; std::string err;
    explicit Parser(const std::string& src) : s(src) {}
    void ws() { while (i < s.size() && isspace((unsigned char)s[i])) ++i; }
    bool term(uintptr_t& v) {
        ws();
        if (i >= s.size()) { err = "unexpected end of expression"; return false; }
        if (s[i] == '[') {
            ++i; uintptr_t inner;
            if (!expr(inner)) return false;
            ws(); if (i >= s.size() || s[i] != ']') { err = "missing ']'"; return false; }
            ++i;
            if (!read_as(inner, v)) { err = "cannot read pointer at " + hexstr(inner); return false; }
            return true;
        }
        if (isdigit((unsigned char)s[i])) {
            size_t st = i; int base = 10;
            if (s.compare(i, 2, "0x") == 0 || s.compare(i, 2, "0X") == 0) { base = 16; i += 2; st = i; }
            while (i < s.size() && isxdigit((unsigned char)s[i])) ++i;
            v = (uintptr_t)strtoull(s.substr(st, i - st).c_str(), nullptr, base);
            return true;
        }
        size_t st = i;
        while (i < s.size() && (isalnum((unsigned char)s[i]) || s[i] == '_' || s[i] == '.' || s[i] == ':' || s[i] == '-')) {
            if (s[i] == '-' && i > st) break;
            ++i;
        }
        std::string sym = s.substr(st, i - st);
        if (sym.empty()) { err = std::string("unexpected character '") + s[i] + "'"; return false; }
        if (_stricmp(sym.c_str(), "game") == 0) { v = game_base(); if (!v) { err = "game module not found"; return false; } return true; }
        v = module_base(sym);
        if (!v) { err = "unknown symbol or module '" + sym + "'"; return false; }
        return true;
    }
    bool expr(uintptr_t& v) {
        if (!term(v)) return false;
        for (;;) {
            ws();
            if (i < s.size() && (s[i] == '+' || s[i] == '-')) {
                char op = s[i++]; uintptr_t r;
                if (!term(r)) return false;
                v = op == '+' ? v + r : v - r;
            } else return true;
        }
    }
};
}

bool eval_addr(const std::string& e, uintptr_t& out, std::string* err) {
    Parser p(e);
    bool okp = p.expr(out);
    if (okp) { p.ws(); if (p.i != e.size()) { p.err = "unexpected text at position " + std::to_string(p.i); okp = false; } }
    if (!okp && err) *err = "address '" + e + "': " + p.err;
    return okp;
}

// ------------------------------------------------------------------ main thread runner
static DWORD g_main_tid = 0;
static std::atomic<uint64_t> g_ticks{0};
static std::mutex g_q_mtx;
static std::vector<std::function<void()>> g_queue;
static std::vector<std::function<void()>> g_tick_fns;
static LONGLONG g_last_tick_us = 0;
static thread_local bool t_in_drain = false;
// 2026-11-15: microseconds from QueryPerformanceCounter. GetTickCount64 advances in 10-16 ms steps, so the ">= 15 ms"
// tick alternated between ~64 Hz and ~32 Hz (per-tick writes such as the FOV override could judder).
static LONGLONG now_us() {
    static LARGE_INTEGER freq{};
    if (!freq.QuadPart) QueryPerformanceFrequency(&freq);
    LARGE_INTEGER c; QueryPerformanceCounter(&c);
    return (LONGLONG)((c.QuadPart / freq.QuadPart) * 1000000LL + ((c.QuadPart % freq.QuadPart) * 1000000LL) / freq.QuadPart);
}

DWORD main_thread_id() { return g_main_tid; }
uint64_t main_ticks() { return g_ticks.load(); }

void add_main_tick(std::function<void()> fn) { std::lock_guard<std::mutex> lk(g_q_mtx); g_tick_fns.push_back(std::move(fn)); }

static void drain_main() {
    if (g_retired || g_features_off) return;
    if (t_in_drain || GetCurrentThreadId() != g_main_tid) return;
    t_in_drain = true;
    ++g_drain_active;
    std::vector<std::function<void()>> work, ticks;
    {
        std::lock_guard<std::mutex> lk(g_q_mtx);
        work.swap(g_queue);
        const LONGLONG now = now_us();
        if (now - g_last_tick_us >= 15000) { g_last_tick_us = now; ticks = g_tick_fns; }
    }
    for (auto& w : work) { try { w(); } catch (std::exception& e) { log("main-thread task threw: %s", e.what()); } catch (...) { log("main-thread task threw (unknown)"); } }
    if (!ticks.empty()) { ++g_ticks; for (auto& t : ticks) { if (g_features_off) break; try { t(); } catch (std::exception& e) { log("tick threw: %s", e.what()); } catch (...) { log("tick threw (unknown)"); } } }
    --g_drain_active;
    t_in_drain = false;
}

using PeekMessage_t = BOOL(WINAPI*)(LPMSG, HWND, UINT, UINT, UINT);
static PeekMessage_t o_PeekMessageW = nullptr, o_PeekMessageA = nullptr;
static BOOL WINAPI hk_PeekMessageW(LPMSG m, HWND h, UINT a, UINT b, UINT r) { drain_main(); return o_PeekMessageW(m, h, a, b, r); }
static BOOL WINAPI hk_PeekMessageA(LPMSG m, HWND h, UINT a, UINT b, UINT r) { drain_main(); return o_PeekMessageA(m, h, a, b, r); }

struct FindWnd { DWORD pid; HWND best; LONG area; };
static BOOL CALLBACK enum_cb(HWND h, LPARAM lp) {
    auto* f = (FindWnd*)lp; DWORD pid = 0;
    GetWindowThreadProcessId(h, &pid);
    if (pid != f->pid || !IsWindowVisible(h) || GetWindow(h, GW_OWNER)) return TRUE;
    RECT rc; GetWindowRect(h, &rc);
    LONG area = (rc.right - rc.left) * (rc.bottom - rc.top);
    if (area > f->area) { f->area = area; f->best = h; }
    return TRUE;
}
HWND find_game_window() { FindWnd f{ GetCurrentProcessId(), nullptr, 0 }; EnumWindows(enum_cb, (LPARAM)&f); return f.best; }

bool install_main_thread_hook() {
    HWND w = nullptr;
    for (int i = 0; i < 1800 && !w; ++i) { w = find_game_window(); if (!w) Sleep(100); }
    if (!w) { log("Main thread: game window not found after 180 s - main-thread commands disabled."); return false; }
    g_main_tid = GetWindowThreadProcessId(w, nullptr);
    HMODULE u32 = GetModuleHandleA("user32.dll");
    void* pw = GetProcAddress(u32, "PeekMessageW");
    void* pa = GetProcAddress(u32, "PeekMessageA");
    bool okh = MH_CreateHook(pw, &hk_PeekMessageW, (void**)&o_PeekMessageW) == MH_OK &&
               MH_CreateHook(pa, &hk_PeekMessageA, (void**)&o_PeekMessageA) == MH_OK &&
               MH_EnableHook(pw) == MH_OK && MH_EnableHook(pa) == MH_OK;
    log("Main thread hook %s (thread %lu)", okh ? "installed" : "FAILED", g_main_tid);
    // The splash screen owns the first window; the game window appears later on another thread.
    // PeekMessage hooks are process-wide, so following the largest window is just a thread-id switch.
    std::thread([]() {
        for (;;) {
            Sleep(1000);
            if (g_retired || g_features_off) return;
            HWND w2 = find_game_window();
            if (!w2) continue;
            DWORD tid = GetWindowThreadProcessId(w2, nullptr);
            if (tid != g_main_tid) { log("Main thread moved to %lu (game window %p replaced the splash screen)", tid, w2); g_main_tid = tid; }
        }
    }).detach();
    return okh;
}

void post_main(std::function<void()> fn) {
    if (GetCurrentThreadId() == g_main_tid) { fn(); return; }
    std::lock_guard<std::mutex> lk(g_q_mtx);
    g_queue.push_back(std::move(fn));
}

json run_on_main(std::function<json()> fn, DWORD timeout_ms) {
    if (GetCurrentThreadId() == g_main_tid) return fn();
    if (!g_main_tid) return fail("NO_MAIN_THREAD", "Main-thread hook is not installed (game window not found). Check the trainer log.");
    // The event belongs to the shared state: it is closed when the waiter AND the queued task are both done with it
    // (a timed-out wait used to leak it; closing it here instead would let the late task signal a dead handle).
    struct State { HANDLE ev = nullptr; json result; std::mutex m; ~State() { if (ev) CloseHandle(ev); } };
    auto st = std::make_shared<State>();
    st->ev = CreateEventA(nullptr, TRUE, FALSE, nullptr);
    if (!st->ev) return fail("NO_EVENT", "could not create the wait event for the main-thread command");
    {
        std::lock_guard<std::mutex> lk(g_q_mtx);
        g_queue.push_back([st, fn]() {
            json r;
            try { r = fn(); } catch (std::exception& e) { r = fail("EXCEPTION", e.what()); } catch (...) { r = fail("EXCEPTION", "unknown C++ exception"); }
            { std::lock_guard<std::mutex> lk2(st->m); st->result = std::move(r); }
            SetEvent(st->ev);
        });
    }
    if (WaitForSingleObject(st->ev, timeout_ms) != WAIT_OBJECT_0)
        return fail("TIMEOUT", "Game main thread did not run the command within " + std::to_string(timeout_ms) +
                    " ms (loading screen or minimized?). It may still run later.");
    std::lock_guard<std::mutex> lk(st->m);
    return st->result;
}

// ------------------------------------------------------------------ crash diagnostics
static std::atomic<int> g_diag_count{0};
static std::string frame_str(uintptr_t a) {
    std::string m = module_name_of(a);
    if (m.empty()) return hexstr(a);
    return m + "+" + hexstr(a - module_base(m));
}
static std::string cxx_type_name(EXCEPTION_RECORD* er) {
    if (er->NumberParameters < 4) return "?";
    uintptr_t base = er->ExceptionInformation[3], ti = er->ExceptionInformation[2];
    int32_t cta_rva = 0, n = 0, ct_rva = 0, td_rva = 0;
    if (!read_as(ti + 12, cta_rva) || !read_as(base + cta_rva, n) || n <= 0) return "?";
    if (!read_as(base + cta_rva + 4, ct_rva) || !read_as(base + ct_rva + 4, td_rva)) return "?";
    char name[160] = {}; safe_read(base + td_rva + 16, name, sizeof name - 1);
    return name;
}
static thread_local bool t_in_veh = false;
static std::atomic<int> dumps{0};
static std::atomic<int> g_fatal_dumps{0};   // minidumps of own-module / non-AV faults (separate cap, 2026-11-02)
static std::atomic<bool> g_in_reload{false};
// added one more such handler. Set right before the new copy is loaded: this copy's VEH steps aside.
static std::atomic<bool> g_handed_over{false};

// 2026-11-02 crash diag: the old VEH skipped EVERY AV inside native.dll as a "guarded probe" and ignored all
// codes except AV / C++ throw / stack overflow, so a real fault in our code (or a heap-corruption / fail-fast / int3)
// left no trace. Now: walk the faulting stack (RtlVirtualUnwind from the exception context) and treat an AV as a probe
// ONLY when one of OUR frames has a __try scope (C scope table) covering its pc. Everything else of error severity is
// logged once (flushed) with module+RVA frames, and dumped (own cap).
struct FaultWalk { int n = 0; uintptr_t pc[24] = {}; bool probe = false; int cover_idx = -1; };   // cover_idx: w.pc index of the frame whose __try covers the fault
static void walk_fault(const CONTEXT* c0, FaultWalk& w, uintptr_t selfBase) {
    CONTEXT c = *c0;
    __try {
        for (int i = 0; i < 24; ++i) {
            const uintptr_t pc = (uintptr_t)c.Rip;
            if (pc < 0x10000) break;
            w.pc[w.n++] = pc;
            DWORD64 ib = 0;
            PRUNTIME_FUNCTION fe = RtlLookupFunctionEntry(pc, &ib, nullptr);
            if (!fe) {   // leaf function: return address is at [rsp]
                if (c.Rsp < 0x10000) break;
                c.Rip = *(DWORD64*)c.Rsp; c.Rsp += 8; continue;
            }
            PVOID hd = nullptr; DWORD64 est = 0;
            PEXCEPTION_ROUTINE h = RtlVirtualUnwind(UNW_FLAG_EHANDLER, ib, pc, fe, &c, &hd, &est, nullptr);
            if (h && hd && selfBase && ib == selfBase && !w.probe) {
                // __C_specific_handler / __GSHandlerCheck_SEH data = SCOPE_TABLE {u32 Count; {Begin, End, Handler, Jump}[Count]}.
                // C++ EH data (FH3/FH4) starts with a FuncInfo RVA, which fails the Count / range checks below.
                const uint32_t* st = (const uint32_t*)hd;
                const uint32_t cnt = st[0];
                if (cnt >= 1 && cnt <= 64) {
                    const uint32_t rva = (uint32_t)(pc - ib);
                    for (uint32_t k = 0; k < cnt; ++k) {
                        const uint32_t* e = st + 1 + k * 4;
                        if (e[0] < e[1] && e[1] - e[0] < 0x100000 && e[3] != 0 && rva >= e[0] && rva < e[1]) { w.probe = true; w.cover_idx = w.n - 1; break; }
                    }
                }
            }
            if ((uintptr_t)c.Rip == pc) break;
        }
    } __except (EXCEPTION_EXECUTE_HANDLER) {}
}
static void frame_buf(uintptr_t a, char* out, size_t n) {
    HMODULE m = nullptr; char path[MAX_PATH] = "";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, (LPCSTR)a, &m) && m &&
        GetModuleFileNameA(m, path, MAX_PATH)) {
        const char* b = strrchr(path, '\\'); b = b ? b + 1 : path;
        sprintf_s(out, n, "%s+0x%llX", b, (unsigned long long)(a - (uintptr_t)m));
    } else sprintf_s(out, n, "0x%llX", (unsigned long long)a);
}
static LONG CALLBACK veh(EXCEPTION_POINTERS* ep) {
    if (t_in_veh || g_retired || g_handed_over) return EXCEPTION_CONTINUE_SEARCH;
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    const bool cxx = code == 0xE06D7363;
    const bool severe = (code & 0xF0000000u) == 0xC0000000u;       // AV, heap corruption C0000374, illegal instr, div0, stack overflow, assertion C0000420 ...
    if (!cxx && !severe && code != EXCEPTION_BREAKPOINT) return EXCEPTION_CONTINUE_SEARCH;
    t_in_veh = true;
    const uintptr_t at = (uintptr_t)ep->ExceptionRecord->ExceptionAddress;
    uintptr_t sb = 0, se = 0;
    { MODULEINFO mi{}; if (g_self && GetModuleInformation(GetCurrentProcess(), g_self, &mi, sizeof mi)) { sb = (uintptr_t)mi.lpBaseOfDll; se = sb + mi.SizeOfImage; } }
    FaultWalk w; walk_fault(ep->ContextRecord, w, sb);
    const bool ours_at = sb && at >= sb && at < se;
    bool ours_stack = false; uintptr_t first_ours = 0;
    for (int i = 0; i < w.n; ++i) if (sb && w.pc[i] >= sb && w.pc[i] < se) { ours_stack = true; if (!first_ours) first_ours = w.pc[i]; }
    // a guarded probe: an AV covered by a __try in the faulting function or the one of ours right above it (safe_read ->
    // memcpy, seh_call -> game code). 2026-11-04: a __try further up (Trinity's catch-all around DrawOverlay) is NOT a
    int cover_ord = -1;
    if (w.probe) { cover_ord = 0; for (int i = 0; i < w.cover_idx && i < w.n; ++i) if (sb && w.pc[i] >= sb && w.pc[i] < se) ++cover_ord; }
    const bool near_probe = w.probe && cover_ord <= 1;
    const bool outer_caught = w.probe && !near_probe;
    if (code == EXCEPTION_ACCESS_VIOLATION && near_probe && !g_in_reload) { t_in_veh = false; return EXCEPTION_CONTINUE_SEARCH; }
    const bool reload_probe = code == EXCEPTION_ACCESS_VIOLATION && near_probe;
    // dedupe: C++ throws all raise at the same KERNELBASE address, so key them on the throwing frame instead
    const uintptr_t key = cxx ? (first_ours ? first_ours : (w.n > 2 ? w.pc[2] : at)) : at;
    {
        static std::mutex seen_mtx; static std::vector<uintptr_t> seen;
        std::lock_guard<std::mutex> lk(seen_mtx);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) { t_in_veh = false; return EXCEPTION_CONTINUE_SEARCH; }
        if (seen.size() < 512) seen.push_back(key);
    }
    const bool important = !cxx && (ours_at || code != EXCEPTION_ACCESS_VIOLATION || outer_caught);   // never capped (still deduped by address)
    if (!important && g_diag_count.fetch_add(1) >= 60) { t_in_veh = false; return EXCEPTION_CONTINUE_SEARCH; }
    char what[160];
    if (cxx) sprintf_s(what, "C++ throw %s", cxx_type_name(ep->ExceptionRecord).c_str());
    else if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        sprintf_s(what, "access violation (%s 0x%llX)", ep->ExceptionRecord->ExceptionInformation[0] == 0 ? "read" : ep->ExceptionRecord->ExceptionInformation[0] == 1 ? "write" : "exec",
                  (unsigned long long)ep->ExceptionRecord->ExceptionInformation[1]);
    else sprintf_s(what, "exception 0x%08lX", code);
    char otag[160]; sprintf_s(otag, "%s, caught by an OUTER catch-all __try %d of our frames up (overlay?) - locks held below it stay held",
                              ours_at ? "IN native" : "called from native", cover_ord);
    const char* tag = reload_probe ? "guarded read caught by our own __try during a reload - harmless, no dump" : outer_caught ? otag : ours_at ? "IN native, NOT under __try - likely FATAL" : (ours_stack && !cxx) ? "called from native, not under our __try" : cxx ? "C++ throw" : "game-side";
    char chain[1800] = ""; size_t len = 0;
    for (int i = 0; i < w.n && len + 80 < sizeof chain; ++i) { char f[96]; frame_buf(w.pc[i], f, sizeof f); len += sprintf_s(chain + len, sizeof chain - len, " <- %s", f); }
    char atb[96]; frame_buf(at, atb, sizeof atb);
    log("[diag] first-chance %s at %s [%s] (thread %lu%s)%s", what, atb, tag, GetCurrentThreadId(), GetCurrentThreadId() == g_main_tid ? ", game main thread" : "", chain);
    // Minidumps (crashes leave no other trace: the game's own reporter uploads and deletes its dump). bin\crashes\.
    // game-side AVs: first 3 per session (fault_*); own-module / non-AV error codes: own cap of 3 (fatal_*).
    t_in_veh = false;
    return EXCEPTION_CONTINUE_SEARCH;
}
// 2026-11-04: exits that never reach a VEH (std::terminate after an uncaught C++ exception, abort, pure virtual call,
// CRT invalid parameter = fail-fast 0xC0000409) are logged (flushed) before the process goes. These handlers belong to
// OUR static CRT only. std::set_terminate is per thread in the MS CRT, so SIGABRT (process-wide) is the real catch;
// install_thread_terminate_handler() is called on our own long-lived threads too.
static std::string current_exception_text() {
    std::exception_ptr ep = std::current_exception();
    if (!ep) return "no active C++ exception";
    try { std::rethrow_exception(ep); }
    catch (const std::exception& e) { return std::string("std::exception: ") + e.what(); }
    catch (...) { return "non-std C++ exception"; }
    return "?";
}
static std::atomic<bool> g_exiting_logged{false};
static void log_fatal_exit(const char* what) {
    if (g_exiting_logged.exchange(true)) return;
    log("[diag] FATAL %s on thread %lu%s: %s - the game will close now", what, GetCurrentThreadId(),
        GetCurrentThreadId() == g_main_tid ? " (game main thread)" : "", current_exception_text().c_str());
}
static void on_terminate() { log_fatal_exit("std::terminate"); abort(); }
static void on_sigabrt(int) { log_fatal_exit("abort()"); }
static void on_purecall() { log_fatal_exit("pure virtual call"); abort(); }
static void on_invalid_param(const wchar_t*, const wchar_t*, const wchar_t*, unsigned, uintptr_t) {
    log_fatal_exit("CRT invalid parameter (e.g. a *_s buffer overflow)");
    __fastfail(FAST_FAIL_INVALID_ARG);   // same outcome as the CRT default, now with a log line first
}
void install_thread_terminate_handler() { std::set_terminate(on_terminate); }
void install_crash_diagnostics() {
    AddVectoredExceptionHandler(0, veh);
    std::set_terminate(on_terminate);
    signal(SIGABRT, on_sigabrt);
    _set_purecall_handler(on_purecall);
    _set_invalid_parameter_handler(on_invalid_param);
    log("Crash diagnostics on (VEH v2: outer-__try faults logged; terminate/abort/purecall/invalid-param logged)");
}


// ------------------------------------------------------------------ log files (both builds)
// logs\<name>_<yyyymmdd_hhmmss>.log; the newest kLogKeep are kept. Every session starts with kSessionStart and DllMain
// writes kSessionEnd when the game exits normally: a log with "Ready." but no end line belonged to a game that crashed (or
// was killed) - it is copied into logs\crashes\crashes_###.log first (kCrashPerFile per file, never deleted by rotation).
static constexpr size_t kLogKeep = 20;
static constexpr int kCrashPerFile = 20;
static const char kSessionStart[] = "[session] started - crash logs on";
static const char kSessionEnd[] = "[session] game closed normally";
static const char kSessionUnload[] = "[session] trainer unloaded";
static const char kCrashHeader[] = "==================== CRASH ";
static std::atomic<int> g_crash_total{ 0 };
static std::atomic<bool> g_prev_crashed{ false };
static std::string g_crash_dir, g_log_name = "trainer";
static char g_log_path[MAX_PATH] = {};

bool crash_last_session() { return g_prev_crashed.load(); }
int crash_log_count() { return g_crash_total.load(); }
std::string crash_log_dir() { return g_crash_dir; }
std::string log_file_path() { return g_log_path; }

static void rotate_logs(const std::string& dir, const std::string& name) {
    const std::string cur = dir + "\\" + name + ".log", ld = dir + "\\logs";
    CreateDirectoryA(ld.c_str(), nullptr);
    WIN32_FILE_ATTRIBUTE_DATA fa{};
    if (GetFileAttributesExA(cur.c_str(), GetFileExInfoStandard, &fa) && (fa.nFileSizeLow || fa.nFileSizeHigh)) {
        FILETIME lt; SYSTEMTIME st; FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &st);
        char base[96]; sprintf_s(base, "\\%s_%04d%02d%02d_%02d%02d%02d", name.c_str(), st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        for (int n = 1; n < 100; ++n) {                      // same second twice (fast reloads): _2, _3 ...
            const std::string to = ld + base + (n == 1 ? std::string() : "_" + std::to_string(n)) + ".log";
            if (MoveFileA(cur.c_str(), to.c_str())) break;
            if (GetLastError() != ERROR_ALREADY_EXISTS && GetLastError() != ERROR_FILE_EXISTS) break;
        }
    }
    std::vector<std::string> files; WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((ld + "\\" + name + "_*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do files.push_back(fd.cFileName); while (FindNextFileA(h, &fd)); FindClose(h); }
    // older builds named them <name>_<unix seconds>.log: those sort before the dated names and age out first
    std::sort(files.begin(), files.end());
    for (size_t i = 0; i + kLogKeep < files.size(); ++i) DeleteFileA((ld + "\\" + files[i]).c_str());
}
static std::vector<std::string> crash_files() {
    std::vector<std::string> v; WIN32_FIND_DATAA fd{};
    HANDLE h = FindFirstFileA((g_crash_dir + "\\crashes_*.log").c_str(), &fd);
    if (h != INVALID_HANDLE_VALUE) { do v.push_back(fd.cFileName); while (FindNextFileA(h, &fd)); FindClose(h); }
    std::sort(v.begin(), v.end());
    return v;
}
static int count_headers(const std::string& file) {
    std::ifstream f(file); std::string ln; int n = 0;
    while (std::getline(f, ln)) if (ln.rfind(kCrashHeader, 0) == 0) ++n;
    return n;
}
static std::string last_line_with(const std::vector<std::string>& lines, const char* tag) {
    for (auto it = lines.rbegin(); it != lines.rend(); ++it) if (it->find(tag) != std::string::npos) return *it;
    return "(none)";
}
static void archive_crash(const std::string& dir, const std::string& name) {
    g_crash_dir = dir + "\\logs\\crashes";
    CreateDirectoryA((dir + "\\logs").c_str(), nullptr);
    CreateDirectoryA(g_crash_dir.c_str(), nullptr);
    const std::string cur = dir + "\\" + name + ".log";
    std::vector<std::string> lines;
    { std::ifstream f(cur); std::string ln; while (std::getline(f, ln)) lines.push_back(ln); }
    bool started = false, ended = false, ready = false;
    for (auto& l : lines) {
        if (l.find(kSessionStart) != std::string::npos) started = true;
        if (l.find(kSessionEnd) != std::string::npos || l.find(kSessionUnload) != std::string::npos) ended = true;
        if (l.find("] Ready.") != std::string::npos) ready = true;
    }
    if (started && ready && !ended) {
        auto files = crash_files();
        int num = 1, in_file = 0;
        if (!files.empty()) {
            num = atoi(files.back().c_str() + 8);                               // "crashes_" + number
            in_file = count_headers(g_crash_dir + "\\" + files.back());
            WIN32_FILE_ATTRIBUTE_DATA fa{};
            const bool big = GetFileAttributesExA((g_crash_dir + "\\" + files.back()).c_str(), GetFileExInfoStandard, &fa) && fa.nFileSizeLow > 8u * 1024 * 1024;
            if (in_file >= kCrashPerFile || big) { ++num; in_file = 0; }
        }
        char fname[48]; sprintf_s(fname, "\\crashes_%03d.log", num);
        WIN32_FILE_ATTRIBUTE_DATA fa{}; SYSTEMTIME st{};
        if (GetFileAttributesExA(cur.c_str(), GetFileExInfoStandard, &fa)) { FILETIME lt; FileTimeToLocalFileTime(&fa.ftLastWriteTime, &lt); FileTimeToSystemTime(&lt, &st); }
        std::ofstream o(g_crash_dir + fname, std::ios::app);
        char when[64]; sprintf_s(when, "%04d-%02d-%02d %02d:%02d:%02d", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        o << kCrashHeader << (in_file + 1) << " of this file ====================\n"
          << "The game closed unexpectedly: it crashed, or it was force-closed (Task Manager).\n"
          << "Last log line written at: " << when << "\n"
          << "Last error recorded:      " << last_line_with(lines, "[diag]") << "\n"
          << "Last trainer option used: " << last_line_with(lines, "[option]") << "\n"
          << "Last character / world:   " << last_line_with(lines, "new player character") << "\n"
          << "---------- full trainer log of that game session ----------\n";
        for (auto& l : lines) o << l << "\n";
        o << "\n";
        o.close();
        g_prev_crashed = true;
    }
    int total = 0;
    for (auto& f : crash_files()) total += count_headers(g_crash_dir + "\\" + f);
    g_crash_total = total;
}
// End-of-session marker, written from DllMain. Other threads are gone or frozen then and may hold the log mutex or
// the CRT file lock, so this uses a fresh handle and WriteFile only (no locks).
static void write_session_end(const char* what) {
    if (!g_log_path[0]) return;
    HANDLE h = CreateFileA(g_log_path, FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) return;
    SYSTEMTIME t; GetLocalTime(&t);
    char line[160]; const int n = sprintf_s(line, "[%02d:%02d:%02d.%03d] %s\r\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, what);
    DWORD w = 0; if (n > 0) WriteFile(h, line, (DWORD)n, &w, nullptr);
    CloseHandle(h);
}
void session_end_marker(bool process_exit) { write_session_end(process_exit ? kSessionEnd : kSessionUnload); }

// Which non-Windows DLLs are in the game (other mods, overlays, injectors): the first thing to know about a crash report.
// Paths outside the game folder are never written in full (logs get shared, paths can hold the user name).
static void log_mod_dlls() {
    static std::vector<std::string> seen;
    char win[MAX_PATH] = {}; GetWindowsDirectoryA(win, MAX_PATH);
    char exe[MAX_PATH] = {}; GetModuleFileNameA(nullptr, exe, MAX_PATH);
    std::string game_root = exe;
    for (int i = 0; i < 4 && game_root.find_last_of("\\/") != std::string::npos; ++i) game_root = game_root.substr(0, game_root.find_last_of("\\/"));   // ...\Borderlands 4
    auto starts_i = [](const std::string& s, const std::string& p) { return s.size() >= p.size() && _strnicmp(s.c_str(), p.c_str(), p.size()) == 0; };
    std::vector<std::string> fresh;
    HMODULE mods[1024]; DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), mods, sizeof mods, &needed)) return;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 1024; ++i) {
        char path[MAX_PATH] = {};
        if (!GetModuleFileNameExA(GetCurrentProcess(), mods[i], path, MAX_PATH)) continue;
        const std::string p = path, base = p.substr(p.find_last_of("\\/") + 1);
        if (starts_i(p, win) || mods[i] == GetModuleHandleA(nullptr) || mods[i] == g_self) continue;
        // the game's own libraries (engine third-party, plugins, upscalers) are not mods
        if (starts_i(p, game_root + "\\Engine\\") || starts_i(p, game_root + "\\OakGame\\Plugins\\")) continue;
        { static const char* kShipped[] = { "amd_fidelityfx_dx12.dll", "amd_fidelityfx_vk.dll", "nvngx_dlss.dll", "nvngx_dlssg.dll", "nvngx_dlssd.dll",
              "sl.interposer.dll", "sl.common.dll", "sl.dlss.dll", "sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll", "libxess.dll", "libxell.dll" };
          bool shipped = false; for (auto s : kShipped) if (_stricmp(base.c_str(), s) == 0 && starts_i(p, game_root + "\\")) shipped = true;
          if (shipped) continue; }
        if (std::find(seen.begin(), seen.end(), p) != seen.end()) continue;
        seen.push_back(p);
        std::string shown = starts_i(p, game_root + "\\") ? p.substr(game_root.size() + 1) : base + " (outside the game folder)";
        WIN32_FILE_ATTRIBUTE_DATA fa{}; char sz[32] = "";
        if (GetFileAttributesExA(path, GetFileExInfoStandard, &fa)) sprintf_s(sz, " %lu KB", (unsigned long)((fa.nFileSizeLow + 1023) / 1024));
        fresh.push_back(shown + sz);
    }
    if (fresh.empty()) return;
    static bool first = true;
    if (first) { log("[mods] other DLLs in the game (mods, overlays, tools): %zu", fresh.size()); for (auto& f : fresh) log("[mods]   %s", f.c_str()); }
    else for (auto& f : fresh) log("[mods] DLL loaded later: %s", f.c_str());
    first = false;
}
void start_mod_dll_watch() {   // now, then every 10 s
    std::thread([] { while (!g_retired) { try { log_mod_dlls(); } catch (...) {} for (int i = 0; i < 100 && !g_retired; ++i) Sleep(100); } }).detach();
}
static void open_session_log(const std::string& name) {
    g_log_name = name;
    archive_crash(g_cfg.root, name);   // last session crashed? keep its log in logs\crashes
    rotate_logs(g_cfg.root, name);     // keep the last kLogKeep sessions' logs
    strcpy_s(g_log_path, (g_cfg.root + "\\" + name + ".log").c_str());
    g_log_file = _fsopen(g_log_path, "w", _SH_DENYNO);   // readable while the game runs
}
static void log_session_start() {
    SYSTEMTIME st; GetLocalTime(&st);
    log("%s - %04d-%02d-%02d, pid %lu (saved crash logs: %d, in logs\\crashes)", kSessionStart, st.wYear, st.wMonth, st.wDay,
        GetCurrentProcessId(), g_crash_total.load());
    if (g_prev_crashed) log("[session] the previous game session ended unexpectedly - its log was saved in logs\\crashes");
}


} // namespace wb

// ------------------------------------------------------------------ entry
static DWORD WINAPI init_thread(LPVOID) {
    using namespace wb;
    // The trainer opens its files by narrow (ANSI) names: a folder with letters outside the system's code page is
    // used by its short (8.3) name instead; without one the trainer cannot find its files.
    std::string p;
    {
        static wchar_t w[32768], sh[32768];
        GetModuleFileNameW(g_self, w, 32768);
        auto narrow = [](const wchar_t* s, std::string& out) {
            char b[MAX_PATH * 3] = {}; BOOL lost = FALSE;
            const bool utf8 = GetACP() == CP_UTF8;
            if (!WideCharToMultiByte(CP_ACP, utf8 ? 0 : WC_NO_BEST_FIT_CHARS, s, -1, b, sizeof b, nullptr, utf8 ? nullptr : &lost) || lost || strlen(b) >= MAX_PATH - 80) return false;
            out = b; return true;
        };
        if (!narrow(w, p) && !(GetShortPathNameW(w, sh, 32768) && narrow(sh, p))) {
            set_attach_state(5);
            MessageBoxW(nullptr, L"The trainer folder's path is too long or has letters the game cannot read, so the trainer cannot start.\n\nThe BL4Trainer folder must be in the game's OakGame\\Binaries\\Win64 folder. If the game is installed in a folder with special letters, move the game to a Steam library with a short, plain path (Steam > game Properties > Installed Files > Move install folder).",
                        L"Borderlands 4 Trainer", MB_OK | MB_ICONWARNING | MB_TOPMOST);
            return 0;
        }
    }
    g_cfg.root = p.substr(0, p.find_last_of("\\/"));
    std::string leaf = g_cfg.root.substr(g_cfg.root.find_last_of("\\/") + 1);
    if (_stricmp(leaf.c_str(), "loaded") == 0) g_cfg.root = g_cfg.root.substr(0, g_cfg.root.find_last_of("\\/"));
    const std::string logname = "trainer";
    // one trainer per game: dsound.dll loads this copy at start; an old 1.0.x BL4Trainer.exe (or a second copy of the
    // folder) must not load another one on top of it
    { char nm[64]; sprintf_s(nm, "Local\\nativeActive_%lu", GetCurrentProcessId()); g_active_marker = CreateMutexA(nullptr, FALSE, nm);
      if (g_active_marker && GetLastError() == ERROR_ALREADY_EXISTS) { CloseHandle(g_active_marker); g_active_marker = nullptr; return 0; } }
    open_session_log(logname);       // after the check above: a refused second copy must not rotate this copy's log
    set_attach_state(1);
    log("Borderlands 4 Trainer %s starting", kVersion);
    log_session_start();
    install_crash_diagnostics();     // [diag] lines for the crash log (no minidumps in this build)
    load_config();
    sv::apply_pending_at_start();    // queued save edits (bin\save_pending.json) before the game reads the save
    settings_load();                 // settings.json: panel hotkey + panel scale
    if (MH_Initialize() != MH_OK) { log("MinHook init failed - cannot hook."); return 0; }
    install_overlay();               // DX12 hooks as early as possible (catches swapchain creation on early attach)
    install_main_thread_hook();
    panel_init();                    // the trainer panel: panel.json + data\ next to the DLL
    log("Ready. Panel hotkey: %s", hotkey_name(g_settings.hotkey).c_str());
    start_mod_dll_watch();
    return 0;
}

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_DETACH && wb::g_self == h) wb::session_end_marker(reserved != nullptr);
    if (reason == DLL_PROCESS_ATTACH) {
        wb::g_self = h;
        DisableThreadLibraryCalls(h);
        HANDLE t = CreateThread(nullptr, 0, init_thread, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
