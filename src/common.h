// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// Shared declarations for the in-game hook DLL (Borderlands 4, UE 5.5, DX12).
#pragma once
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <cstdint>
#include <string>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include "json.hpp"

using json = nlohmann::json;


namespace wb {

constexpr const char* kVersion = "1.1.2";
constexpr const char* kGameExe = "Borderlands4.exe";

struct Config {
    std::string root;              // folder holding the DLL, config, panel, captures
    int port = 47870;
    bool allow_raw = false;        // FN.CALL / MEM.WRITE / BP / PATCH - the user's switch in config.json
    bool lock_input = true;        // game gets no mouse/keyboard while the panel is open
    int  backup_minutes = 20;      // periodic save backups (0 = only on attach)
    int  tx_cooldown_ms = 400;
    std::string token;
};
extern Config g_cfg;
void load_config();
void save_config();

// ---- user settings (settings.cpp) - bin\settings.json, the single source of truth for the panel hotkey
struct Hotkey { int vk = VK_INSERT; bool ctrl = false, shift = false, alt = false; };
struct Settings {
    Hotkey hotkey;
    bool developer_mode = false;
    float ui_scale = 1.f;          // panel scale 0.75 .. 1.5 (font scale + layout, no font rebuild)
};
extern Settings g_settings;
void settings_load();                          // missing / invalid file = defaults; publishes the hotkey to the overlay
bool settings_save(std::string* err = nullptr); // settings.json.tmp + MoveFileEx replace
bool hotkey_vk_allowed(int vk);                // no mouse buttons / bare modifiers / Escape / Windows keys
std::string key_name(int vk);                  // "INSERT", "F5", "NUM 3", ...
std::string hotkey_name(const Hotkey& h);      // "CTRL + SHIFT + F5"
int hotkey_pack(const Hotkey& h);              // vk | ctrl 0x100 | shift 0x200 | alt 0x400 | valid 0x8000
Hotkey hotkey_unpack(int packed);
void overlay_publish_hotkey(int packed);
void overlay_hotkey_capture_ping();            // overlay.cpp: "waiting for a key" - the panel toggle is ignored meanwhile
inline bool developer_mode() { return !WB_PUBLIC && g_settings.developer_mode; }

// ---- logging ----
void log(const char* fmt, ...);
std::vector<std::string> log_tail(size_t n);
// log files (core.cpp, both builds): logs\ keeps the last 20 sessions; a session that ended without a normal game close
// (crash / killed) is copied into logs\crashes\crashes_###.log at the next start
bool crash_last_session();                     // the previous game session ended unexpectedly
int crash_log_count();                         // crashes saved in logs\crashes
std::string crash_log_dir();
std::string log_file_path();
void set_attach_state(long v);                  // what the loader tells the user (core.cpp)
extern std::atomic<bool> g_overlay_drawn;
void set_attach_state_min(long v);              // only raises it
void session_end_marker(bool process_exit);
void start_mod_dll_watch();                    // "[mods]" lines: other DLLs in the game
void install_crash_diagnostics();
// trainer report (report.cpp, both builds): SETTINGS > Support. Call from a worker thread (asks the game thread).
std::string report_create(std::string* out_path);   // writes logs/reports/trainer_report_<time>.txt, returns the text
bool clipboard_set(const std::string& utf8);
void show_in_explorer(const std::string& path);

// ---- modules / memory ----
struct ModuleInfo { std::string name; uintptr_t base = 0; size_t size = 0; };
std::vector<ModuleInfo> list_modules(bool refresh = false);
std::string module_name_of(uintptr_t addr);     // "" when not inside a module
uintptr_t module_base(const std::string& name, size_t* size = nullptr);
uintptr_t game_base(size_t* size = nullptr);
bool safe_read(uintptr_t addr, void* out, size_t n);
bool safe_write(uintptr_t addr, const void* in, size_t n);
template <class T> bool read_as(uintptr_t a, T& v) { return safe_read(a, &v, sizeof(T)); }
std::string hexstr(uint64_t v);
std::string to_hex(const uint8_t* p, size_t n);
bool parse_pattern(const std::string& s, std::vector<int>& pat);

// Address expressions: "game+0x1234", "[sig:inv_core_global]+0x10", "sig:damage_apply", "0x7FF6..."
// [x] dereferences a pointer. Symbols: game, module names, sig:<name>, player/actor (once mapped).
bool eval_addr(const std::string& expr, uintptr_t& out, std::string* err = nullptr);

struct Region { uintptr_t start; size_t size; bool exec; };
std::vector<Region> readable_regions(uintptr_t base, size_t size);

// ---- main game thread ----
bool install_main_thread_hook();
DWORD main_thread_id();
uint64_t main_ticks();
json run_on_main(std::function<json()> fn, DWORD timeout_ms = 3000);
void add_main_tick(std::function<void()> fn);   // runs every main-thread tick (~15 ms)
void post_main(std::function<void()> fn);       // fire-and-forget on the main thread
json guarded_call(uintptr_t fn, const std::vector<uint64_t>& args, const std::string& ret);
HWND find_game_window();
void install_crash_diagnostics();
HMODULE self_module();
void log_close();
void release_active_marker();
extern std::atomic<bool> g_retired;
extern std::atomic<bool> g_game_exiting;    // the game window is closing: no more game calls (core.cpp, set by the overlay)
extern std::atomic<bool> g_features_off;
extern std::atomic<bool> g_reload_failed;
json cmd_reload(const json& a);

void start_server();
void server_stop();
json dispatch(const json& req);
json ok(json data = json::object(), const std::string& msg = "ok");
json fail(const std::string& code, const std::string& msg);
namespace sv { json cmd(const std::string& name, const json& a); void apply_pending_at_start(); }   // savefile.cpp
namespace items { json cmd(const std::string& name, const json& a); void draw_page(float S); void reload_db(); void draw_missions_page(float S); void draw_enemies_page(float S); void draw_places_page(float S); void draw_gearmaker_page(float S); }   // items.cpp

// ---- signatures (sigs.cpp) - named byte patterns, resolved per game build, editable at runtime
void sigs_init();                                   // loads signatures.json, resolves everything
uintptr_t sig_addr(const std::string& name);        // 0 = unresolved
json sigs_cmd_list(const json& a);
json sigs_cmd_add(const json& a);                   // add/replace + resolve now + save
json sigs_cmd_test(const json& a);                  // try a pattern without saving
json sigs_cmd_resolve(const json& a);               // re-resolve all
std::string game_build_id();
const std::string& game_version();   // "1.0.0.3527" (game_build_id up to '|'), cached
int game_build();                    // last part of the version (3527), 0 if unknown
// ---- update safe mode (sigs.cpp + bin\build_gate.json): on a game version that is not verified, only patterns /
// signature names listed for that version may resolve, so unverified features stay off instead of writing to moved data.
bool sig_gate_allows(std::string_view pattern, const char* name);   // false = blocked on this game version
bool sig_gate_safe_mode();                                          // true = running a not-yet-verified game version
json sig_gate_status();
void sigs_on_change(std::function<void()> fn);       // called (from a worker thread) when new signatures resolve                        // exe size + timestamp + file version

// ---- RTTI (rtti.cpp) - the game ships with MSVC RTTI ("pa::" classes)
std::string rtti_class_of(uintptr_t obj);           // "" when not a polymorphic object
json rtti_cmd_class(const json& a);                 // {addr} -> class name + base classes
json rtti_cmd_find(const json& a);                  // {name} -> vtable(s) for a class
json rtti_cmd_instances(const json& a);             // {name|vtable, max} -> live heap objects
json rtti_cmd_dump(const json& a);                  // {addr, len} -> qwords annotated with classes
json rtti_cmd_list(const json& a);                  // {filter, max} -> class names in the exe

json code_cmd_disasm(const json& a);   // CODE.DISASM {addr, count | to, stop_at_ret}
json code_cmd_func(const json& a);     // CODE.FUNC {addr, unique, direct_only} - all chunks
json code_cmd_xrefs(const json& a);    // CODE.XREFS {target, max, kinds} - callers / readers / writers
json code_cmd_names(const json& a);
std::string name_at(uintptr_t a);

void modfs_init();
json mods_cmd_status(const json& a);   // MODS.STATUS
json mods_cmd_set(const json& a);      // MODS.SET {redirect, trace, save}
json mods_cmd_trace(const json& a);    // MODS.TRACE {filter, max, clear}
json mods_cmd_rescan(const json& a);   // MODS.RESCAN

// ---- hardware breakpoints (bp.cpp) - "what writes/reads/executes here", no code changes
json bp_cmd_set(const json& a);                     // {addr, kind: exec|write|rw, len, max_hits}
json bp_cmd_hits(const json& a);
json bp_cmd_clear(const json& a);
void bp_install();

// ---- guard (guard.cpp) - error 298648703 prevention, write gate, journal, save backups
void guard_init();
bool tx_active();                                   // a game commit is in flight (or just finished)
bool gated_write(uintptr_t addr, const void* data, size_t n, const std::string& who); // write now, or queue until safe
void guard_tick();
json guard_cmd_status(const json& a);
json guard_cmd_journal(const json& a);
json guard_cmd_watch(const json& a);                // re-scan for the validator / error raise sites
json guard_cmd_backup(const json& a);               // save backup now
json guard_cmd_errors(const json& a);
void draw_guard_tab();
json guard_cmd_sites(const json& a);                 // every place the error code is raised in game code               // recorded error events with context
void guard_note_feature(const std::string& name);

// ---- write guard hooks for other modules
void guard_set_external_tx(bool (*fn)());
void guard_tx_enter();                              // engine-layer detours mark a game commit in flight
void guard_tx_leave();

// ---- overlay / panel
bool install_overlay();
json overlay_status();
bool overlay_visible();
void overlay_set_visible(bool v);
// 11-06: transmog page open -> x (client px) of its right edge, 0 = closed. Mouse drags that START right of it go to the game
void overlay_set_page_edge(int px);
void overlay_shutdown();
void overlay_retire();
json request_capture(const std::string& path, bool include_panel, DWORD timeout_ms);
void panel_init();
void panel_draw(bool* visible);
json panel_reload();
json panel_state();
json panel_set(const std::string& id, const json& value);
json panel_show(bool visible, const std::string& tab);
json panel_all_off();
void toast(const std::string& text);
void hud_draw();                                    // BL4 always-on HUD (enemy info + loot radar), render thread, bl4.cpp
bool hud_live();                                    // HUD has fresh marks -> the overlay must draw this frame

json cmd_unload(const json& a);
json companions_list(const json& a);                 // plugins loaded from bin\companions (generic, none shipped)

} // namespace wb
