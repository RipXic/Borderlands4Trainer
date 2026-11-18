// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// Pad buttons shared between the overlay HOST (its XInputGetState detour, trinity\hooks\xinput_hook.cpp) and any
// mask = buttons hidden from the GAME (e.g. L3 while aiming in third person, so the shoulder swap does not sprint).
//
// Layout: 16 bytes, never grows (an already-running host created the mapping with this size). The 4th field is the
// HOST's protocol version (0 = a host older than 2026-11-15 or no host servicing the mapping; kPadShareVersion = raw is
// recorded before masking, for every slot, and cleared when a slot disconnects). New fields are NOT possible without a
// new mapping name.
//
// 2026-11-15: the name was "Local\BL4PadShare_%lu" with a single backslash ("\B" is not an escape, so the real name
// was "LocalBL4PadShare_<pid>"). Fixed to "Local\\BL4PadShare_<pid>". A host that is still running from before this
// fix keeps the old name, so a reloaded client and that host do not share the mapping until the GAME IS RESTARTED
// (the client then falls back to its own XInputGetState polling, as with any older host).
#pragma once
#include <Windows.h>
#include <cstdio>
#include <mutex>
struct PadShare { volatile LONG mask; volatile LONG raw; volatile LONG packet; volatile LONG version; };
static_assert(sizeof(PadShare) == 16, "PadShare is shared with a running host - it must stay 16 bytes");
constexpr LONG kPadShareVersion = 2;
inline PadShare* pad_share() {
    static std::once_flag once;
    static PadShare* p = nullptr;
    std::call_once(once, []() {
        char n[64]; sprintf_s(n, "Local\\BL4PadShare_%lu", GetCurrentProcessId());
        HANDLE m = CreateFileMappingA(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, sizeof(PadShare), n);   // opens it if it exists
        if (!m) return;
        void* v = MapViewOfFile(m, FILE_MAP_ALL_ACCESS, 0, 0, sizeof(PadShare));
        if (!v) { CloseHandle(m); return; }
        MEMORY_BASIC_INFORMATION mbi{};   // never read past a smaller mapping
        if (!VirtualQuery(v, &mbi, sizeof mbi) || mbi.RegionSize < sizeof(PadShare)) { UnmapViewOfFile(v); CloseHandle(m); return; }
        p = (PadShare*)v;   // handle + view kept for the session
    });
    return p;
}
