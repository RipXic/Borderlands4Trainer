// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// Attaches native.dll to Borderlands4.exe. Nothing in the game folder is modified (no ASI, no
// mod manager). Two ways in:
//   nativeLoader.exe            game not running: start it through Steam and attach as soon as its
//                                      graphics DLLs load (before the swapchain exists - overlay works with
//                                      DLSS Frame Generation). Game already running: attach now.
//   nativeLoader.exe --attach   only attach to a running game (overlay needs Frame Generation off)
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <tlhelp32.h>
#include <shellapi.h>
#include <cstdio>
#include <cstdarg>
#include <share.h>
#include <string>
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "advapi32.lib")

static const wchar_t* kGameExe = L"Borderlands4.exe";
static const wchar_t* kDllName = L"native.dll";
static const wchar_t* kSteamUrl = L"steam://rungameid/1285190";

// loader.log next to the loader: what happened on this run, with Windows error codes. When the attach fails the
// trainer never runs, so this is the only record a user can send.
static FILE* g_log = nullptr;
static void logf(const char* fmt, ...) {
    if (!g_log) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fprintf(g_log, "%02d:%02d:%02d.%03d ", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    va_list a; va_start(a, fmt); vfprintf(g_log, fmt, a); va_end(a);
    fputc('\n', g_log); fflush(g_log);
}

static void say(WORD color, const char* tag, const char* msg) {
    HANDLE h = GetStdHandle(STD_OUTPUT_HANDLE);
    SetConsoleTextAttribute(h, color); printf("%s", tag);
    SetConsoleTextAttribute(h, 7); printf(" %s\n", msg);
    logf("%s %s", tag, msg);
}
#define INFO(m) say(8, "  ..", m)
#define GOOD(m) say(10, "  OK", m)
#define BAD(m)  say(12, "FAIL", m)

// Two files of the game are called Borderlands4.exe: the starter in the game folder and the game itself in
// OakGame\Binaries\Win64. Attaching to the starter does nothing in game.
enum ProcKind { kStarter, kUnknown, kGame };
static ProcKind proc_kind(DWORD pid) {
    ProcKind k = kUnknown;
    if (HANDLE p = OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid)) {
        static wchar_t path[32768]; DWORD n = 32768;
        if (QueryFullProcessImageNameW(p, 0, path, &n)) {
            _wcslwr_s(path);
            k = wcsstr(path, L"\\binaries\\win") ? kGame : kStarter;
        }
        CloseHandle(p);
    }
    return k;
}

// the game's process (0 = not running). *starter: a starter process is running (the game is about to appear)
static DWORD find_pid(bool* starter = nullptr) {
    if (starter) *starter = false;
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    if (s == INVALID_HANDLE_VALUE) return 0;
    struct P { DWORD pid, parent; } ps[16]; int n = 0;
    PROCESSENTRY32W e{ sizeof e };
    for (BOOL k = Process32FirstW(s, &e); k && n < 16; k = Process32NextW(s, &e))
        if (_wcsicmp(e.szExeFile, kGameExe) == 0 && e.cntThreads > 0) ps[n++] = { e.th32ProcessID, e.th32ParentProcessID };   // 0 threads = dead shell kept by a handle (after a crash)
    CloseHandle(s);
    DWORD unknown = 0;
    for (int i = 0; i < n; ++i) {
        const ProcKind k = proc_kind(ps[i].pid);
        if (k == kGame) return ps[i].pid;
        if (k == kStarter) { if (starter) *starter = true; continue; }
        // path not readable (protected / elevated): a process started by another Borderlands4.exe is the game
        bool child = false;
        for (int x = 0; x < n; ++x) if (x != i && ps[x].pid == ps[i].parent) child = true;
        if (!unknown || child) unknown = ps[i].pid;
    }
    return unknown;
}

static bool has_module(DWORD pid, const wchar_t* name) {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPMODULE | TH32CS_SNAPMODULE32, pid);
    if (s == INVALID_HANDLE_VALUE) return false;
    MODULEENTRY32W m{ sizeof m }; bool found = false;
    for (BOOL k = Module32FirstW(s, &m); k; k = Module32NextW(s, &m))
        if (_wcsicmp(m.szModule, name) == 0) { found = true; break; }
    CloseHandle(s);
    return found;
}

static bool bridge_active(DWORD pid) {
    char nm[64]; sprintf_s(nm, "Local\\nativeActive_%lu", pid);
    HANDLE h = OpenMutexA(SYNCHRONIZE, FALSE, nm);
    if (h) { CloseHandle(h); return true; }
    return false;
}

// What the DLL in the game says about itself: 1 = started, 2 = graphics hooks installed, 3 = hooks failed,
// 4 = the panel draws on the game's window, 5 = cannot use the trainer folder's name, 6 = panel setup failed. 0 = nothing yet.
static LONG dll_state(DWORD pid) {
    char nm[64]; sprintf_s(nm, "Local\\nativeState_%lu", pid);
    LONG v = 0;
    if (HANDLE m = OpenFileMappingA(FILE_MAP_READ, FALSE, nm)) {
        if (auto p = (volatile LONG*)MapViewOfFile(m, FILE_MAP_READ, 0, 0, sizeof(LONG))) { v = *p; UnmapViewOfFile((const void*)p); }
        CloseHandle(m);
    }
    return v;
}
// wait until the DLL reports at least `want` (or a failure); tells the user what is wrong. true = fine
static bool report_dll(DWORD pid, LONG want, int seconds) {
    LONG v = 0;
    for (int i = 0; i < seconds * 10; ++i) {
        v = dll_state(pid);
        if (v == 3 || v >= want) break;
        if (i % 10 == 9 && find_pid() != pid) { logf("the game process ended"); return false; }
        Sleep(100);
    }
    logf("trainer state in the game: %ld", v);
    if (v == 4 || v == want) return true;
    if (v == 5) BAD("The trainer folder's name has letters the game cannot read. Move the folder to e.g. C:\\BL4Trainer, restart the game and try again.");
    else if (v == 3 || v == 6) BAD("The trainer is in the game but could not set up its panel. Restart the game and start it with BL4Trainer.exe. Send trainer.log if it happens again.");
    else if (v == 2) BAD("The trainer is in the game but has not drawn on the game window yet. If the hotkey does nothing: turn Frame Generation off, or close the game and start it with BL4Trainer.exe.");
    else BAD("The trainer was loaded but did not start. Send trainer.log and loader.log.");
    return false;
}

// Each attach loads its own copy (bin\loaded\native_<time>.dll) so a retired copy and a new one
// never share an address - pointers the game still holds into a retired copy stay valid.
static std::wstring stage_copy(const std::wstring& dir, const std::wstring& dll) {
    std::wstring ld = dir + L"\\loaded";
    CreateDirectoryW(ld.c_str(), nullptr);
    {   // tidy up copies from earlier game sessions (a copy still loaded in a running game cannot be deleted - fine)
        WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW((ld + L"\\native_*.dll").c_str(), &fd);
        if (fh != INVALID_HANDLE_VALUE) { do { DeleteFileW((ld + L"\\" + fd.cFileName).c_str()); } while (FindNextFileW(fh, &fd)); FindClose(fh); }
    }
    SYSTEMTIME t; GetLocalTime(&t);
    wchar_t name[96];
    swprintf_s(name, L"\\native_%04d%02d%02d_%02d%02d%02d_%03d.dll", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond, t.wMilliseconds);
    std::wstring dst = ld + name;
    return CopyFileW(dll.c_str(), dst.c_str(), FALSE) ? dst : L"";
}

static const DWORD kAttachRights = PROCESS_CREATE_THREAD | PROCESS_VM_OPERATION | PROCESS_VM_WRITE | PROCESS_VM_READ | PROCESS_QUERY_INFORMATION;

static bool is_elevated() {
    HANDLE tok = nullptr; TOKEN_ELEVATION e{}; DWORD n = 0;
    if (!OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &tok)) return false;
    const bool ok = GetTokenInformation(tok, TokenElevation, &e, sizeof e, &n) != 0;
    CloseHandle(tok);
    return ok && e.TokenIsElevated;
}

// start this loader again as administrator (Windows asks the user); true when the new copy started
static bool relaunch_elevated(const wchar_t* args) {
    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    SHELLEXECUTEINFOW si{ sizeof si };
    si.lpVerb = L"runas"; si.lpFile = self; si.lpParameters = args; si.nShow = SW_SHOWNORMAL;
    const bool ok = ShellExecuteExW(&si) != 0;
    logf("relaunch as administrator %s (error %lu)", ok ? "started" : "failed", ok ? 0 : GetLastError());
    return ok;
}

static bool can_open(DWORD pid, DWORD* err) {
    HANDLE p = OpenProcess(kAttachRights, FALSE, pid);
    if (!p) { *err = GetLastError(); return false; }
    CloseHandle(p); return true;
}

// Windows refused to open the game. true = an administrator copy of the loader took over (this one should exit).
static bool handle_no_access(DWORD err, bool early) {
    char b[200]; sprintf_s(b, "Windows refused access to Borderlands4.exe (error %lu).", err); BAD(b);
    if (err == ERROR_ACCESS_DENIED && !is_elevated()) {
        INFO("The game is running as administrator, the trainer is not. Restarting the trainer as administrator - click Yes...");
        if (relaunch_elevated(early ? L"--early" : L"--attach")) return true;
        BAD("Not restarted. Right-click BL4Trainer.exe > Run as administrator (or stop running Steam / the game as administrator).");
    } else {
        BAD("Antivirus or security software is probably blocking the trainer. Add the trainer folder as an exception and try again.");
    }
    return false;
}

static bool inject(DWORD pid, const std::wstring& dll) {
    HANDLE p = OpenProcess(kAttachRights, FALSE, pid);
    if (!p) { char b[160]; sprintf_s(b, "Cannot open Borderlands4.exe (error %lu).", GetLastError()); BAD(b); return false; }
    size_t bytes = (dll.size() + 1) * sizeof(wchar_t);
    void* mem = VirtualAllocEx(p, nullptr, bytes, MEM_COMMIT | MEM_RESERVE, PAGE_READWRITE);
    DWORD e_mem = mem ? 0 : GetLastError();
    bool okw = mem && WriteProcessMemory(p, mem, dll.c_str(), bytes, nullptr);
    DWORD e_write = (mem && !okw) ? GetLastError() : 0;
    auto ll = (LPTHREAD_START_ROUTINE)GetProcAddress(GetModuleHandleW(L"kernel32.dll"), "LoadLibraryW");
    HANDLE t = okw ? CreateRemoteThread(p, nullptr, 0, ll, mem, 0, nullptr) : nullptr;
    DWORD e_thread = (okw && !t) ? GetLastError() : 0;
    DWORD code = 0, wait = WAIT_FAILED;
    if (t) { wait = WaitForSingleObject(t, 20000); GetExitCodeThread(t, &code); CloseHandle(t); }
    if (mem && wait != WAIT_TIMEOUT) VirtualFreeEx(p, mem, 0, MEM_RELEASE);   // still in use by a thread that timed out
    CloseHandle(p);
    logf("inject: alloc err %lu, write err %lu, thread err %lu, wait %lu, LoadLibraryW returned %lu", e_mem, e_write, e_thread, wait, code);
    char b[220];
    if (!okw) { sprintf_s(b, "Could not write to the game's memory (error %lu) - antivirus may be blocking the trainer.", e_mem ? e_mem : e_write); BAD(b); return false; }
    if (!t) { sprintf_s(b, "Could not start the trainer inside the game (error %lu) - antivirus may be blocking the trainer.", e_thread); BAD(b); return false; }
    if (wait == WAIT_TIMEOUT) { BAD("The game did not finish loading the trainer within 20 seconds."); return false; }
    if (code == 0) { BAD("The game could not load native.dll - unzip the whole trainer folder again (antivirus may have removed or blocked it)."); return false; }
    return true;
}




int wmain(int argc, wchar_t** argv) {
    SetConsoleTitleW(L"Borderlands 4 Trainer");
    printf("\n  BORDERLANDS 4 TRAINER\n  -----------------------------------------\n\n");
    bool attach_only = argc > 1 && _wcsicmp(argv[1], L"--attach") == 0;
    bool force_early = argc > 1 && _wcsicmp(argv[1], L"--early") == 0;   // administrator restart of an early attach

    wchar_t self[MAX_PATH]; GetModuleFileNameW(nullptr, self, MAX_PATH);
    std::wstring dir = self; dir = dir.substr(0, dir.find_last_of(L"\\/"));
    std::wstring dll = dir + L"\\" + kDllName;
    g_log = _wfsopen((dir + L"\\loader.log").c_str(), L"w", _SH_DENYWR);
    {
        OSVERSIONINFOW v{ sizeof v };
        typedef LONG(WINAPI* RtlGetVersion_t)(OSVERSIONINFOW*);
        if (auto rgv = (RtlGetVersion_t)GetProcAddress(GetModuleHandleW(L"ntdll.dll"), "RtlGetVersion")) rgv(&v);
        logf("loader %s, Windows %lu.%lu build %lu, administrator: %s, args: %d", WB_PUBLIC ? "public" : "dev",
             v.dwMajorVersion, v.dwMinorVersion, v.dwBuildNumber, is_elevated() ? "yes" : "no", argc - 1);
    }
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) { BAD("native.dll is missing next to the loader."); Sleep(4000); return 2; }

    DWORD restart_pid = argc > 2 && _wcsicmp(argv[1], L"--restart") == 0 ? (DWORD)_wtoi(argv[2]) : 0;
    {   // restart.txt (written by the trainer, which starts this loader through Explorer so Steam does not count it as part of the game)
        const std::wstring rf = dir + L"\\restart.txt";
        WIN32_FILE_ATTRIBUTE_DATA fa{};
        if (GetFileAttributesExW(rf.c_str(), GetFileExInfoStandard, &fa)) {
            FILETIME now; GetSystemTimeAsFileTime(&now);
            const ULONGLONG age = (((ULONGLONG)now.dwHighDateTime << 32 | now.dwLowDateTime) - ((ULONGLONG)fa.ftLastWriteTime.dwHighDateTime << 32 | fa.ftLastWriteTime.dwLowDateTime)) / 10000000ULL;
            if (age < 120) { FILE* f = _wfopen(rf.c_str(), L"r"); if (f) { unsigned long p = 0; if (fscanf_s(f, "%lu", &p) == 1) restart_pid = p; fclose(f); } }
            DeleteFileW(rf.c_str());
        }
    }
    if (restart_pid) {   // trainer "apply save edits": wait for the game to close, then start it again
        const DWORD old = restart_pid;
        INFO("Waiting for Borderlands 4 to close (save edits are applied when it starts again)...");
        if (HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, old)) { WaitForSingleObject(ph, 180000); CloseHandle(ph); }
        for (int i = 0; i < 600 && find_pid(); ++i) Sleep(100);
        Sleep(8000);   // let Steam notice the game has closed
    }
    bool starter = false;
    DWORD pid = find_pid(&starter);
    bool early = false;
    if (!pid && starter) {   // the starter is up, the game itself is about to appear
        INFO("Borderlands 4 is starting...");
        for (int i = 0; i < 60 * 40 && !pid; ++i) { Sleep(25); pid = find_pid(); }
        if (pid) early = true;
    }
    if (pid && early) {
    } else if (!pid) {
        if (attach_only) { BAD("Borderlands 4 is not running."); Sleep(3000); return 3; }
        INFO("Borderlands 4 is not running - starting it through Steam...");
        ShellExecuteW(nullptr, L"open", kSteamUrl, nullptr, nullptr, SW_SHOWNORMAL);
        for (int i = 0; i < 180 * 40 && !pid; ++i) {
            Sleep(25); pid = find_pid();
            if (!pid && i > 0 && i % (20 * 40) == 0) ShellExecuteW(nullptr, L"open", kSteamUrl, nullptr, nullptr, SW_SHOWNORMAL);   // Steam ignores a launch right after the game exits
        }
        if (!pid) { BAD("Borderlands 4 did not start within 3 minutes."); Sleep(4000); return 4; }
        early = true;
    } else if (force_early) {
        early = true;   // restarted as administrator right after the game started: still attach before the swapchain
    }
    bool again = false;   // the game restarted itself and this is the second attach
    for (;;) {
    char buf[160]; sprintf_s(buf, "Found Borderlands4.exe (pid %lu)", pid); GOOD(buf);
    if (bridge_active(pid)) {
        GOOD("The trainer is already attached. Press your panel hotkey in game (default INSERT).");
        const LONG st = dll_state(pid);
        logf("trainer state in the game: %ld", st);
        if (st && st != 4) { report_dll(pid, 4, 1); Sleep(12000); }
        Sleep(3000); return 0;
    }

    {   // check access first: without it the DirectX wait below cannot see the game's modules and would wait for nothing
        DWORD err = 0;
        if (!can_open(pid, &err)) {
            if (handle_no_access(err, early)) return 0;
            INFO("Details are in loader.log in the trainer folder.");
            Sleep(15000); return 6;
        }
    }

    if (early) {
        // attach the moment the game loads its graphics stack: the DX12 hook then sees the swapchain being created
        INFO("Waiting for the game to load DirectX...");
        int waited = 0;
        while (!has_module(pid, L"dxgi.dll") && !has_module(pid, L"d3d12.dll") && waited < 120 * 20) { Sleep(50); ++waited; if (!find_pid()) { BAD("Borderlands 4 closed."); return 5; } }
        logf("DirectX %s after %d ms", waited < 120 * 20 ? "loaded" : "NOT seen", waited * 50);
        Sleep(150);
    } else {
        INFO("Game already running - late attach (overlay draws when DLSS Frame Generation is off).");
    }

    {   // one loader per game process (two may be waiting for the same restart)
        char nm[64]; sprintf_s(nm, "Local\\nativeLoad_%lu", pid);
        CreateMutexA(nullptr, FALSE, nm);   // kept until this loader closes
        if (GetLastError() == ERROR_ALREADY_EXISTS) { INFO("Another trainer window is already attaching to this game."); Sleep(3000); return 0; }
    }
    std::wstring copy = stage_copy(dir, dll);
    if (copy.empty()) { BAD("Could not copy native.dll (is the trainer folder read-only?)."); Sleep(4000); return 8; }
    if (!inject(pid, copy)) { BAD("Attach failed - details are in loader.log in the trainer folder."); Sleep(15000); return 7; }
    GOOD(early ? "Trainer attached early (full overlay support)." : "Trainer attached.");
    if (!report_dll(pid, early ? 2 : 4, 20) && find_pid() == pid) { Sleep(15000); return 9; }   // game gone: it may be restarting, see below
    INFO("Press your panel hotkey in game (default INSERT; change it on the SETTINGS page).");

    // After a game patch the game restarts itself ("-relaunch") a minute or so after it started: the new process has no
    // trainer. Stay around for the first minutes and attach to the new one as well.
    INFO("You can play now. This window closes by itself; leave it open until the main menu (the game restarts itself after a game patch).");
    HANDLE ph = OpenProcess(SYNCHRONIZE, FALSE, pid);
    if (!ph) { Sleep(3000); return 0; }
    const DWORD ended = WaitForSingleObject(ph, again ? 3 * 60000 : 5 * 60000);
    CloseHandle(ph);
    if (ended != WAIT_OBJECT_0) return 0;
    INFO("The game closed - waiting up to 3 minutes in case it restarts itself...");
    const DWORD old = pid;
    pid = 0;
    for (int i = 0; i < 180 * 40; ++i) { Sleep(25); pid = find_pid(); if (pid && pid != old) break; pid = 0; }
    if (!pid) { INFO("It did not restart."); Sleep(2000); return 0; }
    INFO("The game restarted - attaching again...");
    early = true; again = true;
    }
}
