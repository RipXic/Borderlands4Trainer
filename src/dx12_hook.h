// Ported from Trinity (MIT License, Copyright (c) 2026 XeTrinityz; vTweak by ReXooGen/Lian).
// https://github.com/XeTrinityz/Trinity - see third_party/Trinity-LICENSE.txt. Edited for the Borderlands 4 Trainer.
#pragma once

namespace trinity::hooks
{
    // Grabs the DX12 present/queue vtable pointers from a throwaway device and
    // detours them with MinHook. Must be called after MH_Initialize().
    bool InstallDX12Hooks();
    bool DX12HooksRetryable(); // the last install failed before anything was hooked
    void RemoveDX12Hooks();
    // new copy attached to this host.
    void RearmDX12Hooks();
}
