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

static DWORD find_pid() {
    HANDLE s = CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0);
    PROCESSENTRY32W e{ sizeof e }; DWORD pid = 0;
    for (BOOL k = Process32FirstW(s, &e); k; k = Process32NextW(s, &e))
        if (_wcsicmp(e.szExeFile, kGameExe) == 0 && e.cntThreads > 0) { pid = e.th32ProcessID; break; }   // 0 threads = dead shell kept by a handle (after a crash)
    CloseHandle(s);
    return pid;
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
    DWORD pid = find_pid();
    bool early = false;
    if (!pid) {
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
    char buf[160]; sprintf_s(buf, "Found Borderlands4.exe (pid %lu)", pid); GOOD(buf);
    if (bridge_active(pid)) { GOOD("The trainer is already attached. Press your panel hotkey in game (default INSERT)."); Sleep(3000); return 0; }

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

    std::wstring copy = stage_copy(dir, dll);
    if (copy.empty()) { BAD("Could not copy native.dll (is the trainer folder read-only?)."); Sleep(4000); return 8; }
    if (!inject(pid, copy)) { BAD("Attach failed - details are in loader.log in the trainer folder."); Sleep(15000); return 7; }
    GOOD(early ? "Trainer attached early (full overlay support)." : "Trainer attached.");
    INFO("Press your panel hotkey in game (default INSERT; change it on the SETTINGS page).");

    Sleep(3000);
    return 0;
}
