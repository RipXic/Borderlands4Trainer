// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// (panel\panel.json's "hotkey" is the UMG F8 panel's and is ignored here; config.json no longer has a toggle key).
//    "feature_hotkeys": {"F5": {"vk": 119, "ctrl": true, ...}, "F6": {"off": true}}}   (only the keys the player changed)
// Missing / invalid file or field = default. Written atomically (settings.json.tmp + MoveFileEx replace).
// The hotkey is also published to the shared input mapping (overlay.cpp) so the copy that owns the overlay - the first
#include "common.h"
#include <fstream>
#include <algorithm>
#include <cmath>
#include <map>

namespace wb {

Settings g_settings;

static std::string settings_path() { return g_cfg.root + "\\settings.json"; }

int hotkey_pack(const Hotkey& h) {
    return (h.vk & 0xFF) | (h.ctrl ? 0x100 : 0) | (h.shift ? 0x200 : 0) | (h.alt ? 0x400 : 0) | 0x8000;
}

Hotkey hotkey_unpack(int p) {
    Hotkey h; h.vk = p & 0xFF; h.ctrl = (p & 0x100) != 0; h.shift = (p & 0x200) != 0; h.alt = (p & 0x400) != 0;
    return h;
}

// keys a panel hotkey may use: no mouse buttons, no bare modifiers, no Escape (cancels the capture), no Windows keys
bool hotkey_vk_allowed(int vk) {
    if (vk <= 0x07 || vk > 0xFE) return false;                   // 1..6 = mouse buttons
    switch (vk) {
    case VK_ESCAPE: case VK_SHIFT: case VK_CONTROL: case VK_MENU: case VK_LSHIFT: case VK_RSHIFT: case VK_LCONTROL:
    case VK_RCONTROL: case VK_LMENU: case VK_RMENU: case VK_LWIN: case VK_RWIN: case VK_APPS: case VK_CAPITAL:
    case VK_NUMLOCK: case VK_SCROLL: case VK_PACKET: case 0xFF:
        return false;
    }
    return true;
}

std::string key_name(int vk) {
    struct N { int vk; const char* n; };
    static const N names[] = {
        { VK_INSERT, "INSERT" }, { VK_DELETE, "DELETE" }, { VK_HOME, "HOME" }, { VK_END, "END" }, { VK_PRIOR, "PAGE UP" },
        { VK_NEXT, "PAGE DOWN" }, { VK_UP, "UP" }, { VK_DOWN, "DOWN" }, { VK_LEFT, "LEFT" }, { VK_RIGHT, "RIGHT" },
        { VK_BACK, "BACKSPACE" }, { VK_TAB, "TAB" }, { VK_RETURN, "ENTER" }, { VK_SPACE, "SPACE" }, { VK_PAUSE, "PAUSE" },
        { VK_SNAPSHOT, "PRINT SCREEN" }, { VK_ESCAPE, "ESC" }, { VK_MULTIPLY, "NUM *" }, { VK_ADD, "NUM +" },
        { VK_SUBTRACT, "NUM -" }, { VK_DECIMAL, "NUM ." }, { VK_DIVIDE, "NUM /" }, { VK_CLEAR, "CLEAR" },
        { VK_OEM_3, "`" }, { VK_OEM_MINUS, "-" }, { VK_OEM_PLUS, "=" }, { VK_OEM_4, "[" }, { VK_OEM_6, "]" },
        { VK_OEM_5, "\\" }, { VK_OEM_1, ";" }, { VK_OEM_7, "'" }, { VK_OEM_COMMA, "," }, { VK_OEM_PERIOD, "." },
        { VK_OEM_2, "/" }, { VK_OEM_102, "<>" },
    };
    for (auto& n : names) if (n.vk == vk) return n.n;
    if (vk >= VK_F1 && vk <= VK_F24) return "F" + std::to_string(vk - VK_F1 + 1);
    if (vk >= VK_NUMPAD0 && vk <= VK_NUMPAD9) return "NUM " + std::to_string(vk - VK_NUMPAD0);
    if ((vk >= '0' && vk <= '9') || (vk >= 'A' && vk <= 'Z')) return std::string(1, (char)vk);
    char buf[64] = {};   // anything else: the keyboard layout's own name
    const UINT sc = MapVirtualKeyA((UINT)vk, MAPVK_VK_TO_VSC);
    if (sc && GetKeyNameTextA((LONG)(sc << 16), buf, sizeof buf) > 0) {
        std::string s = buf; std::transform(s.begin(), s.end(), s.begin(), ::toupper); return s;
    }
    char hx[16]; sprintf_s(hx, "KEY 0x%02X", vk); return hx;
}

std::string hotkey_name(const Hotkey& h) {
    std::string s;
    if (h.ctrl) s += "CTRL + ";
    if (h.shift) s += "SHIFT + ";
    if (h.alt) s += "ALT + ";
    return s + key_name(h.vk);
}

// ---- trainer feature hotkeys: default key name -> packed key (0 = off). Read on the game thread, written by the panel.
static std::mutex g_fk_mtx;
static std::map<std::string, int> g_fk;
static std::atomic<unsigned> g_fk_gen{1};

bool hotkey_parse(const std::string& s0, Hotkey& out) {
    Hotkey h; h.vk = 0; std::string key = s0;
    for (;;) {
        const size_t p = key.find('+'); if (p == std::string::npos || p == 0) break;
        std::string mod = key.substr(0, p); for (auto& c : mod) c = (char)tolower((unsigned char)c);
        while (!mod.empty() && mod.back() == ' ') mod.pop_back();
        if (mod == "ctrl") h.ctrl = true; else if (mod == "shift") h.shift = true; else if (mod == "alt") h.alt = true; else break;
        key = key.substr(p + 1); while (!key.empty() && key[0] == ' ') key.erase(0, 1);
    }
    for (auto& c : key) c = (char)toupper((unsigned char)c);
    if (key.size() >= 2 && key[0] == 'F' && isdigit((unsigned char)key[1])) { const int n = atoi(key.c_str() + 1); if (n >= 1 && n <= 24) h.vk = VK_F1 + n - 1; }
    else if (key.size() == 1 && isalnum((unsigned char)key[0])) h.vk = key[0];
    else if (key.rfind("NUM", 0) == 0 && key.size() == 4 && isdigit((unsigned char)key[3])) h.vk = VK_NUMPAD0 + (key[3] - '0');
    else {
        static const std::pair<const char*, int> m[] = { {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT},
            {"DELETE", VK_DELETE}, {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"PAUSE", VK_PAUSE} };
        for (auto& kv : m) if (key == kv.first) h.vk = kv.second;
    }
    if (!h.vk) return false;
    out = h; return true;
}

std::string hotkey_short(const Hotkey& h) {
    std::string s;
    if (h.ctrl) s += "Ctrl+";
    if (h.shift) s += "Shift+";
    if (h.alt) s += "Alt+";
    return s + key_name(h.vk);
}

int feature_key(const std::string& def) {
    { std::lock_guard<std::mutex> lk(g_fk_mtx); auto it = g_fk.find(def); if (it != g_fk.end()) return it->second; }
    Hotkey h; return hotkey_parse(def, h) ? hotkey_pack(h) : 0;
}
bool feature_key_changed(const std::string& def) { std::lock_guard<std::mutex> lk(g_fk_mtx); return g_fk.count(def) != 0; }
void feature_key_set(const std::string& def, int packed) {
    { std::lock_guard<std::mutex> lk(g_fk_mtx);
      Hotkey d; const int dp = hotkey_parse(def, d) ? hotkey_pack(d) : 0;
      if (packed < 0 || packed == dp) g_fk.erase(def); else g_fk[def] = packed; }
    ++g_fk_gen;
}
unsigned feature_keys_gen() { return g_fk_gen.load(); }

void settings_load() {
    Settings s;   // defaults
    std::map<std::string, int> fk;
    std::ifstream f(settings_path(), std::ios::binary);
    if (f) {
        try {
            json j = json::parse(f);
            if (j.is_object()) {
                auto hk = j.find("hotkey");
                if (hk != j.end() && hk->is_object()) {
                    const int vk = hk->value("vk", VK_INSERT);
                    if (hotkey_vk_allowed(vk)) {
                        s.hotkey.vk = vk;
                        s.hotkey.ctrl = hk->value("ctrl", false); s.hotkey.shift = hk->value("shift", false); s.hotkey.alt = hk->value("alt", false);
                    } else log("[settings] hotkey vk %d is not allowed - using INSERT", vk);
                }
                auto us = j.find("ui_scale");
                if (us != j.end() && us->is_number()) { const double v = us->get<double>(); if (std::isfinite(v)) s.ui_scale = (float)std::clamp(v, 0.75, 1.5); }
                auto fj = j.find("feature_hotkeys");
                if (fj != j.end() && fj->is_object())
                    for (auto& kv : fj->items()) {
                        const json& v = kv.value(); if (!v.is_object()) continue;
                        if (v.value("off", false)) { fk[kv.key()] = 0; continue; }
                        Hotkey h; h.vk = v.value("vk", 0); h.ctrl = v.value("ctrl", false); h.shift = v.value("shift", false); h.alt = v.value("alt", false);
                        if (hotkey_vk_allowed(h.vk)) fk[kv.key()] = hotkey_pack(h);
                    }
            }
        } catch (std::exception& e) {
            log("[settings] settings.json is not valid (%s) - using defaults", e.what());
            s = Settings(); fk.clear();
        }
    }
    g_settings = s;
    { std::lock_guard<std::mutex> lk(g_fk_mtx); g_fk = fk; } ++g_fk_gen;
    if (!fk.empty()) log("[settings] %d trainer hotkey(s) moved or switched off", (int)fk.size());
    overlay_publish_hotkey(hotkey_pack(g_settings.hotkey));
    log("[settings] panel hotkey %s, ui scale %.2f%s", hotkey_name(g_settings.hotkey).c_str(),
        g_settings.ui_scale, f ? "" : " (no settings.json - defaults)");
}

bool settings_save(std::string* err) {
    json j = {
        {"hotkey", { {"vk", g_settings.hotkey.vk}, {"ctrl", g_settings.hotkey.ctrl}, {"shift", g_settings.hotkey.shift}, {"alt", g_settings.hotkey.alt} }},
        {"ui_scale", std::round(g_settings.ui_scale * 100.0) / 100.0},
    };
    {   json fj = json::object(); std::lock_guard<std::mutex> lk(g_fk_mtx);
        for (auto& kv : g_fk) {
            if (!kv.second) { fj[kv.first] = { {"off", true} }; continue; }
            const Hotkey h = hotkey_unpack(kv.second);
            fj[kv.first] = { {"vk", h.vk}, {"ctrl", h.ctrl}, {"shift", h.shift}, {"alt", h.alt} };
        }
        if (!fj.empty()) j["feature_hotkeys"] = fj;
    }
    overlay_publish_hotkey(hotkey_pack(g_settings.hotkey));
    const std::string path = settings_path(), tmp = path + ".tmp";
    {
        std::ofstream f(tmp, std::ios::binary | std::ios::trunc);
        if (!f) { if (err) *err = "cannot write " + tmp; log("[settings] cannot write %s", tmp.c_str()); return false; }
        f << j.dump(2) << "\n";
        f.flush();
        if (!f) { if (err) *err = "write failed: " + tmp; return false; }
    }
    if (!MoveFileExA(tmp.c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        const DWORD e = GetLastError();
        DeleteFileA(tmp.c_str());
        if (err) *err = "cannot replace settings.json (error " + std::to_string(e) + ")";
        log("[settings] cannot replace %s (error %lu)", path.c_str(), e);
        return false;
    }
    return true;
}

} // namespace wb
