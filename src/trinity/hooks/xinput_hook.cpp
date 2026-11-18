// Ported from Trinity (MIT License, Copyright (c) 2026 XeTrinityz; vTweak by ReXooGen/Lian).
// https://github.com/XeTrinityz/Trinity - see third_party/Trinity-LICENSE.txt. Edited for the Borderlands 4 Trainer.
#include "xinput_hook.h"
#include "dualsense.h"

#include <atomic>

#include <MinHook.h>

#include "../core/state.h"
#include "../../pad_share.h"

#pragma comment(lib, "xinput9_1_0.lib")

namespace trinity::hooks
{
    using XInputGetState_t = DWORD (WINAPI*)(DWORD, XINPUT_STATE*);

    // One trampoline per known module. The game and our own overlay may link
    // different xinput versions, and each hook must forward to its own original.
    static XInputGetState_t o_1_4   = nullptr;
    static XInputGetState_t o_1_3   = nullptr;
    static XInputGetState_t o_9_1_0 = nullptr;

    // Any real-state trampoline, used by the menu to read the pad it's blocking.
    static XInputGetState_t g_read = nullptr;

    static void Neutralize(XINPUT_STATE* s)
    {
        // Only strip the buttons the menu itself uses (d-pad, A/B/X, and the
        // LB/RB section switchers). The thumbsticks, triggers and every other
        // button stay live so the player can still move and look around with
        // the menu open.
        constexpr WORD kMenuButtons =
            XINPUT_GAMEPAD_DPAD_UP  | XINPUT_GAMEPAD_DPAD_DOWN |
            XINPUT_GAMEPAD_DPAD_LEFT | XINPUT_GAMEPAD_DPAD_RIGHT |
            XINPUT_GAMEPAD_A | XINPUT_GAMEPAD_B | XINPUT_GAMEPAD_X |
            XINPUT_GAMEPAD_LEFT_SHOULDER | XINPUT_GAMEPAD_RIGHT_SHOULDER;

        if (s->Gamepad.wButtons & kMenuButtons)
        {
            s->Gamepad.wButtons &= ~kMenuButtons;
            ++s->dwPacketNumber; // bump so the game registers the state as changed
        }
    }

    // pad_share bookkeeping, shared by all three detours (one array: the game, the trainer and Steam can each poll
    // through a different xinput module). Last raw buttons per slot, BEFORE the mask is applied.
    static std::atomic<WORD> g_padLast[4];
    // Bumped by every detour's post-processing on this thread. xinput9_1_0 can forward to xinput1_4 through the hooked
    // export: the outer detour then sees that the inner one already recorded / masked / neutralised and steps aside
    // (otherwise it would record the already-masked buttons as "raw").
    static thread_local unsigned t_padSeq = 0;

    static DWORD AfterRead(DWORD i, XINPUT_STATE* s, DWORD r)
    {
        if (r != ERROR_SUCCESS && i == 0 && dualsense::IsConnected() && s)
        {
            if (dualsense::GetState(s))
                r = ERROR_SUCCESS;
        }
        const bool okState = (r == ERROR_SUCCESS && s);
        if (i < 4)
        {
            if (PadShare* ps = pad_share())
            {
                if (ps->version != kPadShareVersion)
                    InterlockedExchange(&ps->version, kPadShareVersion);
                // a slot that fails (pad switched off / dropped) reports no buttons - nothing stays "held"
                const WORD now = okState ? s->Gamepad.wButtons : static_cast<WORD>(0);
                if (g_padLast[i].exchange(now) != now)
                    InterlockedIncrement(&ps->packet);
                const WORD all = static_cast<WORD>(g_padLast[0].load() | g_padLast[1].load() |
                                                   g_padLast[2].load() | g_padLast[3].load());
                InterlockedExchange(&ps->raw, static_cast<LONG>(all));
                if (okState)
                {
                    const WORD m = static_cast<WORD>(ps->mask);
                    if (m) s->Gamepad.wButtons &= static_cast<WORD>(~m);
                }
            }
        }
        if (okState && State::Get().menuOpen)
            Neutralize(s);
        return r;
    }

    // A distinct detour per module so each can call the matching trampoline
    // (MinHook can't tell us which target a shared detour was invoked for).
    #define TRINITY_XINPUT_DETOUR(NAME, ORIG)                              \
        static DWORD WINAPI NAME(DWORD i, XINPUT_STATE* s)                 \
        {                                                                  \
            const unsigned seq = t_padSeq;                                 \
            const DWORD r = ORIG(i, s);                                    \
            if (seq != t_padSeq)                                           \
                return r; /* a nested detour already handled this read */  \
            ++t_padSeq;                                                    \
            return AfterRead(i, s, r);                                     \
        }
    TRINITY_XINPUT_DETOUR(hk_1_4,   o_1_4)
    TRINITY_XINPUT_DETOUR(hk_1_3,   o_1_3)
    TRINITY_XINPUT_DETOUR(hk_9_1_0, o_9_1_0)
    #undef TRINITY_XINPUT_DETOUR

    struct Target
    {
        const wchar_t*    dll;
        void*             detour;
        XInputGetState_t* original;
        bool              done; // hooked, or confirmed nothing to hook here
        int               fails = 0; // MH_CreateHook / MH_EnableHook failures (gives up after a few)
    };

    static Target g_targets[] = {
        { L"xinput1_4.dll",   reinterpret_cast<void*>(&hk_1_4),   &o_1_4,   false },
        { L"xinput1_3.dll",   reinterpret_cast<void*>(&hk_1_3),   &o_1_3,   false },
        { L"xinput9_1_0.dll", reinterpret_cast<void*>(&hk_9_1_0), &o_9_1_0, false },
    };

    void EnsureXInputHooks()
    {
        dualsense::Start();

        static bool s_allDone = false;
        if (s_allDone)
            return;

        // Called every frame from the present thread: look for the modules at most once a second (GetModuleHandle
        // takes the loader lock), and stop for good after ~2 minutes - a module that is not loaded by then is not
        // used by this game (xinput1_3.dll normally never loads).
        static ULONGLONG s_nextTry = 0;
        static int       s_passes  = 0;
        const ULONGLONG nowMs = GetTickCount64();
        if (nowMs < s_nextTry)
            return;
        s_nextTry = nowMs + 1000;
        constexpr int kMaxPasses = 120;
        constexpr int kMaxFails  = 5;

        bool anyPending = false;
        for (auto& t : g_targets)
        {
            if (t.done)
                continue;

            HMODULE mod = GetModuleHandleW(t.dll);
            if (!mod)
            {
                anyPending = true; // may still load (game inits input after us)
                continue;
            }

            FARPROC proc = GetProcAddress(mod, "XInputGetState");
            if (!proc)
            {
                t.done = true; // module present but no export - never retry it
                continue;
            }

            const MH_STATUS created = MH_CreateHook(reinterpret_cast<void*>(proc), t.detour,
                                                    reinterpret_cast<void**>(t.original));
            if (created != MH_OK && created != MH_ERROR_ALREADY_CREATED) // already created = enabling failed last pass
            {
                if (++t.fails >= kMaxFails)
                    t.done = true;     // cannot be hooked (e.g. a forwarder stub) - stop trying
                else
                    anyPending = true; // maybe transient; try again on the next pass
                continue;
            }

            // Publish the reader before enabling so a concurrent XInputReadReal
            // never falls through to a now-patched plain export.
            if (!g_read)
                g_read = *t.original;

            const MH_STATUS enabled = MH_EnableHook(reinterpret_cast<void*>(proc));
            if (enabled == MH_OK || enabled == MH_ERROR_ENABLED)
            {
                // of this copy; these are created lazily, after install_overlay marked the DX12 hooks persistent).
                MH_MarkPersistent(reinterpret_cast<void*>(proc));
                t.done = true;
            }
            else
            {
                // created but not enabled: the next pass retries the enable (the trampoline stays valid for g_read)
                if (++t.fails >= kMaxFails)
                    t.done = true;
                else
                    anyPending = true;
            }
        }

        if (!anyPending || ++s_passes >= kMaxPasses)
            s_allDone = true;
    }

    // host afterwards the pad detours are needed again.
    void RearmXInputHooks()
    {
        for (auto& t : g_targets)
        {
            if (!t.done || !*t.original)
                continue;
            if (HMODULE mod = GetModuleHandleW(t.dll))
                if (FARPROC proc = GetProcAddress(mod, "XInputGetState"))
                    MH_EnableHook(reinterpret_cast<void*>(proc)); // MH_ERROR_ENABLED when it is still on - fine
        }
    }

    void RemoveXInputHooks()
    {
        dualsense::Stop();

        // The detours are torn down by MH_DisableHook(MH_ALL_HOOKS) /
        // MH_Uninitialize during shutdown; just reset our bookkeeping.
        for (auto& t : g_targets)
        {
            t.done      = false;
            *t.original = nullptr;
        }
        g_read = nullptr;
    }

    // XInputGetState on an EMPTY slot is slow (device enumeration) and the menu polls every frame: an empty slot is
    // re-probed only every 1.5 s.
    static std::atomic<ULONGLONG> g_slotRetryAt[4];

    DWORD XInputReadReal(DWORD userIndex, XINPUT_STATE* state)
    {
        DWORD r = ERROR_DEVICE_NOT_CONNECTED;
        const ULONGLONG nowMs = GetTickCount64();
        if (userIndex >= 4 || nowMs >= g_slotRetryAt[userIndex].load())
        {
            const XInputGetState_t rd = g_read;
            if (rd)
                r = rd(userIndex, state);
            else
                r = XInputGetState(userIndex, state); // hooks not up yet
            if (userIndex < 4)
                g_slotRetryAt[userIndex].store(r == ERROR_SUCCESS ? 0 : nowMs + 1500);
        }

        if (r != ERROR_SUCCESS && userIndex == 0 && dualsense::IsConnected() && state)
        {
            if (dualsense::GetState(state))
                r = ERROR_SUCCESS;
        }
        return r;
    }

    // The first connected pad in any slot (the pad is not always slot 0).
    DWORD XInputReadRealAny(XINPUT_STATE* state)
    {
        for (DWORD i = 0; i < 4; ++i)
            if (XInputReadReal(i, state) == ERROR_SUCCESS)
                return ERROR_SUCCESS;
        return ERROR_DEVICE_NOT_CONNECTED;
    }

    bool IsDualSenseConnected()
    {
        return dualsense::IsConnected();
    }

    const char* GetDualSenseName()
    {
        return dualsense::GetDeviceName();
    }
}
