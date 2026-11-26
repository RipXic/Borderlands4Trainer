// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// Borderlands 4 Trainer drop-in loader. Built as dsound.dll and placed next to the game exe
// (<game>\OakGame\Binaries\Win64\dsound.dll) together with the BL4Trainer\ folder.
// The game loads dsound.dll by itself at startup (it imports it), so no separate program has to start or attach anything:
//   1. every DirectSound call is passed on unchanged to the real Windows dsound.dll (System32), so game sound works -
//      or to dsound_chain.dll when the user kept another mod's dsound.dll under that name;
//   2. before the game's own code starts, it loads the user's other mods the way the common ASI loaders do:
//      *.asi in Win64, Win64\scripts and Win64\plugins, and *.dll in Win64\mods (alphabetical order), plus the
//      Borderlands 4 SDK (Win64\Plugins\*.dll) when its own dsound.dll was replaced;
//   3. a small thread loads BL4Trainer\native.dll once Windows has finished loading the game's DLLs.
// Each start writes BL4Trainer\loaded_mods.log (file names only). Uninstall: delete dsound.dll and the BL4Trainer folder.
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <algorithm>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>

static HMODULE g_self = nullptr;
static HMODULE g_sys = nullptr;     // System32 dsound.dll
static HMODULE g_chain = nullptr;   // Win64\dsound_chain.dll (another mod's dsound.dll, renamed), if any
static std::wstring g_dir;          // this dll's folder (the game's Win64)
static bool g_game = false;         // running inside Borderlands4.exe
static const wchar_t* kGameExe = L"Borderlands4.exe";

// ------------------------------------------------------------------ log (file names only, no user folders)
static void mlog(const std::wstring& line, bool fresh = false) {
    const std::wstring dir = g_dir + L"\\BL4Trainer";
    if (GetFileAttributesW(dir.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    FILE* f = nullptr;
    if (_wfopen_s(&f, (dir + L"\\loaded_mods.log").c_str(), fresh ? L"w, ccs=UTF-8" : L"a, ccs=UTF-8") || !f) return;
    SYSTEMTIME t; GetLocalTime(&t);
    fwprintf(f, L"%02d:%02d:%02d.%03d %s\n", t.wHour, t.wMinute, t.wSecond, t.wMilliseconds, line.c_str());
    fclose(f);
}

// ------------------------------------------------------------------ dsound_chain.dll
// Another mod's dsound.dll kept as dsound_chain.dll: loaded once (outside DllMain), and sound calls go through it.
// A renamed copy of this loader is skipped (it would call itself forever); checked without running its DllMain.
static INIT_ONCE g_chain_once = INIT_ONCE_STATIC_INIT;
static BOOL CALLBACK chain_init(PINIT_ONCE, PVOID, PVOID*) {
    if (!g_game || g_dir.empty()) return TRUE;
    const std::wstring p = g_dir + L"\\dsound_chain.dll";
    if (GetFileAttributesW(p.c_str()) == INVALID_FILE_ATTRIBUTES) return TRUE;
    if (HMODULE peek = LoadLibraryExW(p.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES)) {
        const bool ours = GetProcAddress(peek, "BL4TrainerProxy") != nullptr;
        FreeLibrary(peek);
        if (ours) { mlog(L"skipped dsound_chain.dll (it is a copy of this loader)"); return TRUE; }
    }
    g_chain = LoadLibraryW(p.c_str());
    if (g_chain) mlog(L"loaded dsound_chain.dll (sound calls go through it)");
    else mlog(L"FAILED dsound_chain.dll (error " + std::to_wstring(GetLastError()) + L")");
    return TRUE;
}
static void init_chain() { InitOnceExecuteOnce(&g_chain_once, chain_init, nullptr, nullptr); }

static FARPROC real(const char* name) {
    init_chain();
    if (g_chain) if (FARPROC f = GetProcAddress(g_chain, name)) return f;
    if (!g_sys) {
        wchar_t sys[MAX_PATH]; UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        if (!n || n >= MAX_PATH) return nullptr;
        g_sys = LoadLibraryW((std::wstring(sys) + L"\\dsound.dll").c_str());
        if (!g_sys) return nullptr;
    }
    return GetProcAddress(g_sys, name);
}

typedef UINT_PTR U;
#define DSERR_GENERIC_ ((HRESULT)0x80004005L)
#define FWD(name, ...) auto f = (HRESULT(WINAPI*)(__VA_ARGS__))real(name); if (!f) return DSERR_GENERIC_;

extern "C" {
HRESULT WINAPI p_DirectSoundCreate(U a, U b, U c)                { FWD("DirectSoundCreate", U, U, U) return f(a, b, c); }
HRESULT WINAPI p_DirectSoundCreate8(U a, U b, U c)               { FWD("DirectSoundCreate8", U, U, U) return f(a, b, c); }
HRESULT WINAPI p_DirectSoundEnumerateA(U a, U b)                 { FWD("DirectSoundEnumerateA", U, U) return f(a, b); }
HRESULT WINAPI p_DirectSoundEnumerateW(U a, U b)                 { FWD("DirectSoundEnumerateW", U, U) return f(a, b); }
HRESULT WINAPI p_DirectSoundCaptureCreate(U a, U b, U c)         { FWD("DirectSoundCaptureCreate", U, U, U) return f(a, b, c); }
HRESULT WINAPI p_DirectSoundCaptureCreate8(U a, U b, U c)        { FWD("DirectSoundCaptureCreate8", U, U, U) return f(a, b, c); }
HRESULT WINAPI p_DirectSoundCaptureEnumerateA(U a, U b)          { FWD("DirectSoundCaptureEnumerateA", U, U) return f(a, b); }
HRESULT WINAPI p_DirectSoundCaptureEnumerateW(U a, U b)          { FWD("DirectSoundCaptureEnumerateW", U, U) return f(a, b); }
HRESULT WINAPI p_GetDeviceID(U a, U b)                           { FWD("GetDeviceID", U, U) return f(a, b); }
HRESULT WINAPI p_DllGetClassObject(U a, U b, U c)                { FWD("DllGetClassObject", U, U, U) return f(a, b, c); }
HRESULT WINAPI p_DirectSoundFullDuplexCreate(U a, U b, U c, U d, U e, U g, U h, U i, U j, U k) {
    FWD("DirectSoundFullDuplexCreate", U, U, U, U, U, U, U, U, U, U) return f(a, b, c, d, e, g, h, i, j, k);
}
HRESULT WINAPI p_DllCanUnloadNow() {
    if (!g_sys && !g_chain) return S_FALSE;
    auto f = (HRESULT(WINAPI*)())real("DllCanUnloadNow");
    return f ? f() : S_FALSE;
}
// Marks this file as the Borderlands 4 Trainer loader (so a renamed copy is never chained to itself).
int WINAPI p_BL4TrainerProxy() { return 1; }
}

// ------------------------------------------------------------------ the user's other mods
static void load_folder(const std::wstring& sub, const wchar_t* pattern, DWORD flags = 0) {
    const std::wstring dir = sub.empty() ? g_dir : g_dir + L"\\" + sub;
    std::vector<std::wstring> names;
    WIN32_FIND_DATAW fd; HANDLE fh = FindFirstFileW((dir + L"\\" + pattern).c_str(), &fd);
    if (fh == INVALID_HANDLE_VALUE) return;
    do { if (!(fd.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY)) names.push_back(fd.cFileName); } while (FindNextFileW(fh, &fd));
    FindClose(fh);
    std::sort(names.begin(), names.end(), [](const std::wstring& a, const std::wstring& b) { return _wcsicmp(a.c_str(), b.c_str()) < 0; });
    for (const auto& n : names) {
        const std::wstring shown = sub.empty() ? n : sub + L"\\" + n;
        if (LoadLibraryExW((dir + L"\\" + n).c_str(), nullptr, flags)) mlog(L"loaded " + shown);
        else mlog(L"FAILED " + shown + L" (error " + std::to_wstring(GetLastError()) + L")");
    }
}
// The Borderlands 4 SDK (oak2 mod manager) normally comes with its own dsound.dll ("pluginloader") that loads every
// *.dll in Win64\Plugins. When the trainer's dsound.dll replaced it, load them here the same way, so the SDK and its
// console still start. Skipped when the SDK is already loaded (its pluginloader was kept as dsound_chain.dll).
static void load_sdk_plugins() {
    const std::wstring sdk = g_dir + L"\\Plugins\\unrealsdk.dll";
    if (GetFileAttributesW(sdk.c_str()) == INVALID_FILE_ATTRIBUTES) return;
    if (GetModuleHandleW(sdk.c_str())) { mlog(L"Borderlands 4 SDK: already loaded by dsound_chain.dll"); return; }
    mlog(L"Borderlands 4 SDK found in Plugins - loading it (like its own plugin loader)");
    load_folder(L"Plugins", L"*.dll", LOAD_LIBRARY_SEARCH_DEFAULT_DIRS | LOAD_LIBRARY_SEARCH_DLL_LOAD_DIR);
}
static volatile LONG g_mods_done = 0;
static void load_mods(const wchar_t* when) {
    if (InterlockedExchange(&g_mods_done, 1)) return;
    mlog(std::wstring(L"Borderlands 4 Trainer loader - loading other mods (") + when + L")");
    init_chain();
    load_sdk_plugins();
    load_folder(L"", L"*.asi");
    load_folder(L"scripts", L"*.asi");
    load_folder(L"plugins", L"*.asi");
    load_folder(L"mods", L"*.dll");
    mlog(L"done");
}

// Loading mods before the game's own code runs, the way ASI loaders do: the exe's entry point is replaced by a jump
// to entry_hook for one call; entry_hook puts the original bytes back, loads the mods and then runs the real entry.
static BYTE* g_entry = nullptr;
static BYTE g_saved[14];
static void write_code(BYTE* at, const BYTE* src, size_t n) {
    DWORD old; VirtualProtect(at, n, PAGE_EXECUTE_READWRITE, &old);
    memcpy(at, src, n);
    VirtualProtect(at, n, old, &old);
    FlushInstructionCache(GetCurrentProcess(), at, n);
}
static DWORD WINAPI entry_hook(LPVOID peb) {
    write_code(g_entry, g_saved, sizeof g_saved);
    load_mods(L"game start");
    return ((DWORD(WINAPI*)(LPVOID))g_entry)(peb);
}
static bool hook_entry() {
    BYTE* base = (BYTE*)GetModuleHandleW(nullptr);
    auto nt = (IMAGE_NT_HEADERS*)(base + ((IMAGE_DOS_HEADER*)base)->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || !nt->OptionalHeader.AddressOfEntryPoint) return false;
    g_entry = base + nt->OptionalHeader.AddressOfEntryPoint;
    DWORD old; if (!VirtualProtect(g_entry, sizeof g_saved, PAGE_EXECUTE_READWRITE, &old)) return false;
    VirtualProtect(g_entry, sizeof g_saved, old, &old);
    memcpy(g_saved, g_entry, sizeof g_saved);
    BYTE jmp[14] = { 0xFF, 0x25, 0, 0, 0, 0 };   // jmp [rip+0] ; <absolute address>
    void* to = (void*)&entry_hook; memcpy(jmp + 6, &to, sizeof to);
    write_code(g_entry, jmp, sizeof jmp);
    return true;
}

// ------------------------------------------------------------------ the trainer
// Runs after the loader lock is released (a thread created in DllMain starts only then), so LoadLibrary is safe here.
static DWORD WINAPI load_trainer(LPVOID) {
    if (!g_game) return 0;                                           // only inside the game itself
    std::wstring dll = g_dir + L"\\BL4Trainer\\native.dll";
    if (GetFileAttributesW(dll.c_str()) == INVALID_FILE_ATTRIBUTES) return 0;
    LoadLibraryW(dll.c_str());                                       // native.dll refuses a second copy by itself
    return 0;
}
static DWORD WINAPI late_mods(LPVOID) { load_mods(L"late start"); return 0; }

BOOL APIENTRY DllMain(HMODULE h, DWORD reason, LPVOID reserved) {
    if (reason == DLL_PROCESS_ATTACH) {
        g_self = h;
        DisableThreadLibraryCalls(h);
        static wchar_t p[32768];
        if (GetModuleFileNameW(nullptr, p, 32768)) {
            std::wstring exe = p; exe = exe.substr(exe.find_last_of(L"\\/") + 1);
            g_game = _wcsicmp(exe.c_str(), kGameExe) == 0;
        }
        if (GetModuleFileNameW(h, p, 32768)) { g_dir = p; g_dir = g_dir.substr(0, g_dir.find_last_of(L"\\/")); }
        if (!g_game) return TRUE;                                    // any other program: sound pass-through only
        mlog(L"Borderlands 4 Trainer loader started", true);
        // reserved != null: loaded with the game at startup (its entry has not run yet). Loaded later, or the entry could
        // not be hooked: load the mods from a thread instead.
        if (!reserved || !hook_entry()) {
            HANDLE m = CreateThread(nullptr, 0, late_mods, nullptr, 0, nullptr);
            if (m) CloseHandle(m);
        }
        HANDLE t = CreateThread(nullptr, 0, load_trainer, nullptr, 0, nullptr);
        if (t) CloseHandle(t);
    }
    return TRUE;
}
