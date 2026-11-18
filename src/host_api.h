// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// (DX12 overlay, ImGui context, fonts, item icons, input capture, panel toggle key). Every later copy
// and draws them through the host's ImGui context.
#pragma once
#include <cstdint>
#include "imgui.h"

namespace wb {

struct HostIcons {
    bool (*item)(ImDrawList*, const char*, ImVec2, ImVec2, ImU32);
    void (*icon)(ImDrawList*, int, ImVec2, ImVec2, ImU32);
    ImVec2 (*size)(int, float);
    bool (*ready)();
};

struct HostApi {
    uint32_t magic;            // 'CBHA'
    uint32_t abi;              // bump when this struct changes
    int imgui_version;
    uint32_t io_size, style_size, drawlist_size, font_size;
    ImGuiContext* (*context)();
    void (*allocators)(ImGuiMemAllocFunc*, ImGuiMemFreeFunc*, void**);
    ImFont* (*font)(int which);          // 0 body, 1 bold, 2 title
    float (*ui_scale)();
    bool (*visible)();
    void (*set_visible)(bool);
    void (*toast)(const char*);
    void (*set_client)(void (*draw)(bool*));   // nullptr = stop drawing the client
    HostIcons icons;
};

constexpr uint32_t kHostMagic = 0x41484243;   // "CBHA"
constexpr uint32_t kHostAbi = 1;
// kHostAbi stays 1 on purpose (2026-11-15 review): the struct layout did not change. Bumping it for WB_HUD_ONLY would
// make every client built before the bump take its old "no usable host -> install my own overlay" path inside a new
// host (a second overlay; that path is only fixed in builds from 2026-11-15 on). Instead the host protects itself: a
// client callback that faults - e.g. an old client that takes WB_HUD_ONLY for a real bool* - is detached and refused
// (overlay.cpp client_fault / h_set_client). Newer host<->client behaviour is negotiated through the InputShare mapping
// (overlay.cpp: reserved[3] client HUD state, reserved[4] host capability), which needs no ABI change.
// BUMP kHostAbi whenever the HostApi struct itself changes.
// font(which): 0 body, 1 bold, 2 title, 3 skin label, 4 skin heading.
// set_client's draw(vis): vis == WB_HUD_ONLY = draw only the HUD layer (enemy info, loot radar, Matrix code rain)
#define WB_HUD_ONLY ((bool*)(uintptr_t)1)

const HostApi* host_api();      // the host this copy draws through; nullptr in the host itself
const HostIcons* host_icons();  // icon routing for the engine layer's icons.cpp (nullptr in the host)
bool host_connect();            // client: find the host and share its ImGui context
void host_drop_client();        // a retiring client: the host stops drawing it
void host_attach_panel();       // a started client: the host draws its panel from now on

} // namespace wb
