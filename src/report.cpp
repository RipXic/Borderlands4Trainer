// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// One button makes a text file (logs\reports\trainer_report_<time>.txt, also copied to the clipboard) to post where the
// user gets support. The first lines give a verdict, the BOT DATA block at the end is for a support bot:
//   OK      - every trainer check passed: the issue is most likely something else on the PC
//   CHECK   - nothing broken right now, but an earlier game session ended unexpectedly (crash log saved)
//   PROBLEM - a trainer check failed (engine core, overlay, errors in this session's log, a game build the trainer was
//             not made for) -> flagged for a fix
// Crashes come from the crash logs (core.cpp: a session log with no end marker is kept in logs\crashes).
// Privacy: the Windows user name, the PC name, the user profile path, the trainer and game folders are replaced.
// Runs on a worker thread (never the render thread): it asks the game thread for the trainer state through dk::cmd.
#include "common.h"
#include "dk.h"
#include "ue.h"
#include <Windows.h>
#include <shellapi.h>
#include <algorithm>
#include <cctype>
#include <filesystem>
#include <fstream>
#include <sstream>

#pragma comment(lib, "version.lib")
#pragma comment(lib, "advapi32.lib")   // GetUserNameA (scrub)

namespace wb {

namespace fs = std::filesystem;

// The game build this trainer release was made and tested for (Borderlands4.exe size in bytes -> the build the main menu
// shows bottom left). Update both when the game updates.
static const uint64_t kMadeForExe = 647338264ULL;
static const struct { uint64_t size; const char* version; } kKnownBuilds[] = {
    { 647338264ULL, "1.11.0-4906557" },
};

static std::string now_text() {
    SYSTEMTIME t; GetLocalTime(&t); char b[40];
    sprintf_s(b, "%04d-%02d-%02d %02d:%02d:%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); return b;
}
static std::string now_stamp() {
    SYSTEMTIME t; GetLocalTime(&t); char b[40];
    sprintf_s(b, "%04d%02d%02d_%02d%02d%02d", t.wYear, t.wMonth, t.wDay, t.wHour, t.wMinute, t.wSecond); return b;
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)std::tolower((unsigned char)c); return s; }
static void replace_all_ci(std::string& s, const std::string& what, const std::string& with) {
    if (what.size() < 3) return;
    const std::string lw = lower(what);
    std::string ls = lower(s); size_t p = 0;
    while ((p = ls.find(lw, p)) != std::string::npos) { s.replace(p, what.size(), with); ls.replace(p, what.size(), lower(with)); p += with.size(); }
}
static std::string scrub(std::string s) {
    char buf[MAX_PATH] = {};
    std::string game_dir;
    if (GetModuleFileNameA(nullptr, buf, MAX_PATH)) { game_dir = buf; for (int i = 0; i < 4 && game_dir.find_last_of("\\/") != std::string::npos; ++i) game_dir = game_dir.substr(0, game_dir.find_last_of("\\/")); }
    replace_all_ci(s, g_cfg.root, "<trainer folder>");
    if (game_dir.size() > 3) replace_all_ci(s, game_dir, "<game folder>");
    if (const char* up = getenv("USERPROFILE")) replace_all_ci(s, up, "<user folder>");
    DWORD n = MAX_PATH; if (GetUserNameA(buf, &n) && strlen(buf) >= 3) replace_all_ci(s, buf, "<user>");
    n = MAX_PATH; if (GetComputerNameA(buf, &n) && strlen(buf) >= 3) replace_all_ci(s, buf, "<pc>");
    return s;
}
static std::string windows_version() {
    typedef LONG(WINAPI* RtlGetVersion_t)(PRTL_OSVERSIONINFOW);
    RTL_OSVERSIONINFOW v = { sizeof v };
    if (auto f = (RtlGetVersion_t)GetProcAddress(GetModuleHandleA("ntdll.dll"), "RtlGetVersion")) f(&v);
    char b[96]; sprintf_s(b, "Windows %s (build %lu)", v.dwBuildNumber >= 22000 ? "11" : "10", v.dwBuildNumber); return b;
}
static void game_exe(uint64_t& size, std::string& file_version) {
    size = 0; file_version = "?";
    char p[MAX_PATH]; if (!GetModuleFileNameA(nullptr, p, MAX_PATH)) return;
    WIN32_FILE_ATTRIBUTE_DATA a; if (GetFileAttributesExA(p, GetFileExInfoStandard, &a)) size = ((uint64_t)a.nFileSizeHigh << 32) | a.nFileSizeLow;
    DWORD h = 0, n = GetFileVersionInfoSizeA(p, &h); if (!n) return;
    std::vector<char> buf(n); VS_FIXEDFILEINFO* fi = nullptr; UINT fl = 0;
    if (GetFileVersionInfoA(p, 0, n, buf.data()) && VerQueryValueA(buf.data(), "\\", (void**)&fi, &fl) && fi) {
        char b[48]; sprintf_s(b, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS)); file_version = b;
    }
}
static bool noteworthy(const std::string& line) {   // log lines that point at a trainer problem
    const std::string l = lower(line);
    if (l.find("[report]") != std::string::npos || l.find("[option]") != std::string::npos || l.find("[mods]") != std::string::npos
        || l.find("crash diagnostics on") != std::string::npos || l.find("[session]") != std::string::npos) return false;
    if (l.find("[diag]") != std::string::npos) return l.find("game-side") == std::string::npos && l.find("c++ throw") == std::string::npos;   // the game's own handled faults are noise
    static const char* keys[] = { "failed", "error", "exception", "crashed", "gave up", "refused", "fatal", "threw",
                                  "not found", "cannot", "can't", "invalid", "unexpectedly", "timed out", "timeout" };
    for (auto k : keys) if (l.find(k) != std::string::npos) return true;
    return false;
}
// the newest crash in logs\crashes: its header lines (what was last used, last error)
static std::vector<std::string> newest_crash_summary(std::string* when) {
    std::vector<std::string> out; const std::string dir = crash_log_dir(); if (dir.empty()) return out;
    std::error_code ec; fs::path newest; fs::file_time_type best{};
    for (auto& e : fs::directory_iterator(dir, ec)) { const auto t = fs::last_write_time(e.path(), ec); if (newest.empty() || t > best) { best = t; newest = e.path(); } }
    if (newest.empty()) return out;
    std::ifstream f(newest); std::string l; std::vector<std::string> block;
    while (std::getline(f, l)) { if (l.rfind("==================== CRASH ", 0) == 0) block.clear(); block.push_back(l); }
    for (auto& b : block) {
        if (b.rfind("Last ", 0) == 0) out.push_back(b);
        if (when && b.rfind("Last log line written at:", 0) == 0) *when = b.substr(26);
        if (b.rfind("----------", 0) == 0) break;
    }
    // and the last 25 lines of that session's log
    std::vector<std::string> tail; bool in_log = false;
    for (auto& b : block) { if (in_log && !b.empty()) tail.push_back(b); if (b.rfind("----------", 0) == 0) in_log = true; }
    if (tail.size() > 25) tail.erase(tail.begin(), tail.end() - 25);
    out.push_back("last lines of that session's log:");
    for (auto& t : tail) out.push_back("  " + t);
    return out;
}

// ------------------------------------------------------------------ the report
std::string report_create(std::string* out_path) {
    json problems = json::array(), checks = json::array(), notes = json::array();
    std::ostringstream body;
    auto check = [&](const std::string& name, bool ok, const std::string& detail) {
        checks.push_back({ {"check", name}, {"ok", ok}, {"detail", detail} });
        body << "  [" << (ok ? " OK " : "FAIL") << "] " << name << (detail.empty() ? "" : " - " + detail) << "\n";
        if (!ok) problems.push_back(name + (detail.empty() ? "" : ": " + detail));
    };

    uint64_t exe_size = 0; std::string exe_ver; game_exe(exe_size, exe_ver);
    std::string game_version = "unknown build (" + std::to_string(exe_size) + " bytes)";
    for (auto& b : kKnownBuilds) if (b.size == exe_size) game_version = b.version;
    std::string made_for = "?"; for (auto& b : kKnownBuilds) if (b.size == kMadeForExe) made_for = b.version;

    // live state (the game thread answers through dk::cmd; a loading screen can make it time out)
    json ue_st = json::object(), ov = json::object(), st = json::object(), tg = json::object(), vals = json::object();
    try { ue_st = ue::status(); } catch (...) {}
    try { ov = overlay_status(); } catch (...) {}
    try { json r = dk::cmd("BL.STATE", json::object()); if (r.value("ok", false)) st = r["data"]; } catch (...) {}
    try { json r = dk::cmd("BL.TOGGLES", json::object()); if (r.value("ok", false)) tg = r["data"]; } catch (...) {}
    try { json r = dk::cmd("BL.VALUES", json::object()); if (r.value("ok", false)) vals = r["data"]; } catch (...) {}
    const bool in_game = st.value("in_game", false);
    const bool solo = st.value("standalone", false);
    const json player = st.value("player", json::object());

    body << "CHECKS\n";
    check("Game build", exe_size == kMadeForExe, exe_size == kMadeForExe ? game_version
          : "this game build (" + game_version + ") is not the one this trainer was made for (" + made_for + ") - the trainer may need an update");
    { const bool ok = ue_st.value("ready", false);
      check("Engine core (game internals found)", ok, ok ? "ready" : "not ready yet - load a character, or the game updated"); }
    if (ov.value("role", std::string()).find("client") != std::string::npos)
        check("Overlay and input", true, "handled by the first trainer copy in this game (developer reload)");
    else { const bool hooks = ov.value("hooks", false); const uint64_t frames = ov.value("frames_drawn", (uint64_t)0);
      check("Overlay (panel drawing)", hooks && frames > 0, hooks ? std::to_string(frames) + " frames drawn" : "graphics hooks failed");
      check("Input", ov.value("input_attached", false), ov.value("input_attached", false) ? "keyboard and mouse reach the panel" : "the panel is not attached to the game window"); }
    if (in_game) check("Character loaded", true, "level " + std::to_string((int)player.value("level", 0.0)) + (solo ? ", solo" : ", not solo"));
    else {
        notes.push_back("No character was loaded when the report was made - load your character first for a complete report.");
        body << "  [ -- ] Character - none loaded (load one and make the report again for the full check)\n";
    }
    if (in_game && !solo) notes.push_back("Not a solo session: the trainer's options work in single player only.");

    // this session's log
    std::vector<std::string> warn;
    for (auto& l : log_tail(800)) if (noteworthy(l)) { std::string s = scrub(l); if (std::find(warn.begin(), warn.end(), s) == warn.end()) warn.push_back(s); }
    if (warn.size() > 30) warn.erase(warn.begin(), warn.end() - 30);
    if (!warn.empty()) problems.push_back(std::to_string(warn.size()) + " warning/error line(s) in this session's trainer log");

    // crashes
    std::string crash_when; std::vector<std::string> crash = crash_log_count() > 0 ? newest_crash_summary(&crash_when) : std::vector<std::string>();
    if (crash_last_session()) notes.push_back("The previous game session ended unexpectedly (crash or force-close) - see LAST CRASH.");

    std::string verdict = !problems.empty() ? "PROBLEM" : crash_last_session() ? "CHECK" : "OK";
    const std::string verdict_text =
        verdict == "OK" ? "TRAINER OK - every trainer check passed. If something is wrong, it is most likely not the trainer." :
        verdict == "CHECK" ? "TRAINER CHECK - the trainer works right now, but the last game session ended unexpectedly." :
                             "TRAINER PROBLEM - a trainer check failed (see below). This is flagged for a fix.";

    // options switched on + edited numbers
    std::string active;
    for (auto& kv : tg.items()) { const json a = kv.value().value("active", json(false));
        if (a.is_boolean() && !a.get<bool>()) continue;
        if (!active.empty()) active += ", "; active += kv.key();
        if (a.is_number() && a.get<double>() != 1.0) { char b[32]; sprintf_s(b, "=%g", a.get<double>()); active += b; } }
    const int edits = vals.contains("overrides") && vals["overrides"].is_array() ? (int)vals["overrides"].size() : 0;

    std::ostringstream out;
    out << "=== BORDERLANDS 4 TRAINER REPORT ===\n"
        << verdict_text << "\n\n"
        << "Trainer:   Borderlands 4 Trainer " << kVersion
        << "\n"
        << "Game:      Borderlands 4 " << game_version << " (exe " << exe_ver << ")\n"
        << "Made for:  " << made_for << "\n"
        << "System:    " << windows_version() << "\n"
        << "Session:   " << (in_game ? std::string(solo ? "solo" : "not solo") + ", character loaded" : "no character loaded") << "\n"
        << "Made:      " << now_text() << "\n\n"
        << body.str() << "\n";
    if (!notes.empty()) { out << "NOTES\n"; for (auto& n : notes) out << "  - " << n.get<std::string>() << "\n"; out << "\n"; }
    out << "OPTIONS SWITCHED ON\n  " << (active.empty() ? "(none)" : active) << "\n";
    if (edits) out << "  + " << edits << " edited game number(s) (Gear Maker perk values)\n";
    out << "\nWARNINGS AND ERRORS IN THIS SESSION'S LOG\n";
    if (warn.empty()) out << "  (none)\n"; else for (auto& w : warn) out << "  " << w << "\n";
    out << "\n";
    if (!crash.empty()) {
        out << "LAST CRASH (" << crash_log_count() << " saved in logs\\crashes)\n";
        for (auto& c : crash) out << "  " << scrub(c) << "\n";
        out << "\n";
    }
    out << "LAST LINES OF THIS SESSION'S LOG\n";
    for (auto& l : log_tail(40)) out << "  " << scrub(l) << "\n";
    out << "\n";

    json bot = { {"kind", "trainer_report"}, {"format", 1}, {"game", "borderlands4"}, {"trainer", "Borderlands 4 Trainer"},
                 {"trainer_version", kVersion}, {"build", WB_PUBLIC ? "public" : "dev"}, {"verdict", verdict},
                 {"game_version", game_version}, {"made_for", made_for}, {"game_build_match", exe_size == kMadeForExe},
                 {"exe_size", exe_size}, {"windows", windows_version()}, {"character_loaded", in_game}, {"solo", solo},
                 {"problems", problems}, {"notes", notes}, {"checks", checks}, {"log_warnings", (int)warn.size()},
                 {"crash_logs", crash_log_count()}, {"last_session_crashed", crash_last_session()}, {"made", now_text()} };
    out << "----- BOT DATA (please do not edit) -----\n" << bot.dump() << "\n----- END BOT DATA -----\n";

    const std::string text = scrub(out.str());
    std::error_code ec; const std::string dir = g_cfg.root + "\\logs\\reports";
    fs::create_directories(dir, ec);
    const std::string path = dir + "\\trainer_report_" + now_stamp() + ".txt";
    { std::ofstream f(path, std::ios::binary); f << text; }
    { std::vector<fs::path> all; for (auto& e : fs::directory_iterator(dir, ec)) all.push_back(e.path());   // newest 10 kept
      std::sort(all.begin(), all.end()); while (all.size() > 10) { fs::remove(all.front(), ec); all.erase(all.begin()); } }
    log("[report] trainer report written: %s (verdict %s)", fs::path(path).filename().string().c_str(), verdict.c_str());
    if (out_path) *out_path = path;
    return text;
}

bool clipboard_set(const std::string& utf8) {
    const int wn = MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, nullptr, 0); if (wn <= 0) return false;
    if (!OpenClipboard(nullptr)) return false;
    EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, (size_t)wn * sizeof(wchar_t)); bool ok = false;
    if (h) { if (void* p = GlobalLock(h)) { MultiByteToWideChar(CP_UTF8, 0, utf8.c_str(), -1, (wchar_t*)p, wn); GlobalUnlock(h); ok = SetClipboardData(CF_UNICODETEXT, h) != nullptr; } if (!ok) GlobalFree(h); }
    CloseClipboard(); return ok;
}

void show_in_explorer(const std::string& path) {
    const std::string args = "/select,\"" + path + "\"";
    ShellExecuteA(nullptr, "open", "explorer.exe", args.c_str(), nullptr, SW_SHOWNORMAL);
}

} // namespace wb
