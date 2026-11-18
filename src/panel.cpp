// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.

//
//   {title, hotkey, tabs:[{name, sections:[{name, items:[{id, type, label, desc, ...}]}]}]}
//   types: toggle | slider {min,max,step,default,off} | button {button, danger} | readout | input | label/note {text}
//   anything else is shown by its label. The spec is re-read when the file changes (checked every second).
//
//   req.json   we write (req.json.tmp + rename) a JSON array of requests, only when req.json does not exist; otherwise
//                {"seq":N,"op":"set","id":"god","v":true}          toggles / sliders (slider: on release or 250 ms idle)
//                {"seq":N,"op":"click","id":"gold10k","confirmed":false}   buttons (danger: confirmed after our own
//                                                                           second click within 4 s)
//                {"seq":N,"op":"all_off"}
//                {t, seq_done, results:{"<seq>":{ok,res}}, values:{id:v}, readouts:{id:"text"}, keeper:{...},
//                 player:{...}, panel_log:[...]}
//              values are the source of truth for toggles/sliders (synced unless the user is dragging / a request is
// File IO runs on a small worker thread (never on the game or render thread). Paths can be overridden in
// bin\config.json: "project_root", "panel_spec", "panel_ipc".
#include "common.h"
#include "ui_kit.h"
#include "dk.h"
#include "ue.h"
#include <fstream>
#include <map>
#include <deque>
#include <set>
#include <algorithm>
#include <thread>
#include <cmath>
#include <functional>
#include <ctime>
#include <shellapi.h>
#include "imgui.h"
#include "imgui_internal.h"
#include "trinity/gui/icons.h"

#define LINK "Game connected"
#define LINK_OFF "Waiting for the game"
#define LINK_FOOT "LIVE"
#define LINK_FOOT_OFF "WAITING"

namespace wb {

extern ImFont* g_font_body;
extern ImFont* g_font_bold;
extern ImFont* g_font_title;
extern float g_ui_scale;
#define S(x) ((x) * g_ui_scale)

struct Item {
    // from the spec
    std::string id, type, label, desc, button, text;
    std::string hotkey;             // skin: key badge shown on the row ("F5")
    bool danger = false;
    double minv = 0, maxv = 1, step = 0, defv = 0, offv = 0;
    bool has_off = false;
    // live
    bool on = false;
    double val = 0;
    bool have_value = false;
    bool dirty = false;             // slider moved, not sent yet
    ULONGLONG last_edit = 0;
    ULONGLONG last_vlog = 0;        // last "[option] .. = value" log line (sliders send often: at most one per 5 s)
    ULONGLONG confirm_until = 0;    // danger button: second click before this = confirmed
    int64_t last_seq = 0;
    ULONGLONG sent_at = 0;
    bool waiting = false;           // request sent, no result yet
    std::string result;
    bool result_ok = true;
    ULONGLONG result_at = 0;
    std::string readout;
    char input[256] = {};
    // "choice" (drop-down): options [{label, value}] from the spec; the chosen value is sent like an input
    std::vector<std::pair<std::string, std::string>> options;
    int choice = 0;
    bool free_text = false;         // choice: Enter with no matching option sends the typed words
    char filter[128] = {};          // choice: search box in the open list (lists of more than 12)
};
struct Section { std::string name; std::vector<Item> items; };
struct Tab { std::string name, page; std::vector<Section> sections; };   // page: built-in page ("items") instead of sections

static std::recursive_mutex g_mtx;
static std::vector<Tab> g_tabs;
static std::string g_title = "BORDERLANDS 4 TRAINER";
static std::string g_load_msg;
static int g_tab = 0;
static char g_search[128] = {};
static FILETIME g_spec_mtime{};
static std::string g_proj, g_spec_path, g_ipc_dir;
static std::string g_drag_id;           // slider being dragged this frame (render thread)

struct Link {
    bool ever = false;
    FILETIME mtime{};
    int64_t seq_done = 0;
    std::string version, err;
    json keeper = json::object(), player = json::object(), panel_log = json::array(), selfheal = json::object();
    ULONGLONG reads = 0;
};
static Link g_link;
static std::deque<json> g_queue;        // requests waiting for req.json to be free
static int64_t g_seq = 0;
static uint64_t g_written = 0;          // req.json files written this session
static std::string g_status;            // last action result (footer)
static bool g_status_ok = true;
static ULONGLONG g_status_at = 0;
static std::atomic<bool> g_worker_on{false};
static bool g_force_state = false;

// The callback runs with g_mtx held (worker thread when the result arrives / times out, or right away when offline).
using CallCb = std::function<void(bool ok, const json& res)>;
struct PendingCall { std::string fn; ULONGLONG sent_at = 0; CallCb cb; };
static std::map<int64_t, PendingCall> g_calls;
static const ULONGLONG kCallTimeoutMs = 8000;

// ------------------------------------------------------------------ helpers
static double jnum(const json& j, const char* k, double d) {
    auto it = j.find(k); return (it != j.end() && it->is_number()) ? it->get<double>() : d;
}
static std::string jstr(const json& j, const char* k, const std::string& d = std::string()) {
    auto it = j.find(k); return (it != j.end() && it->is_string()) ? it->get<std::string>() : d;
}
static bool jbool(const json& j, const char* k, bool d) {
    auto it = j.find(k); if (it == j.end()) return d;
    if (it->is_boolean()) return it->get<bool>();
    if (it->is_number()) return it->get<double>() != 0;
    return d;
}
static std::string as_text(const json& v) {
    if (v.is_string()) return v.get<std::string>();
    if (v.is_null()) return "-";
    if (v.is_number_float()) { char b[64]; double d = v.get<double>(); if (d == std::floor(d) && std::fabs(d) < 1e15) sprintf_s(b, "%.0f", d); else sprintf_s(b, "%g", d); return b; }
    return v.dump(-1, ' ', false, json::error_handler_t::replace);
}
static bool file_mtime(const std::string& p, FILETIME& ft) {
    WIN32_FILE_ATTRIBUTE_DATA d; if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &d)) return false;
    ft = d.ftLastWriteTime; return true;
}
static double age_seconds(const FILETIME& ft) {
    FILETIME now; GetSystemTimeAsFileTime(&now);
    ULARGE_INTEGER a, b; a.LowPart = ft.dwLowDateTime; a.HighPart = ft.dwHighDateTime; b.LowPart = now.dwLowDateTime; b.HighPart = now.dwHighDateTime;
    return (double)(int64_t)(b.QuadPart - a.QuadPart) / 1e7;
}
static bool lua_online() { return g_link.ever && age_seconds(g_link.mtime) < 5.0; }
static std::string parent_dir(const std::string& p) { size_t k = p.find_last_of("\\/"); return k == std::string::npos ? p : p.substr(0, k); }
static bool exists(const std::string& p) { return GetFileAttributesA(p.c_str()) != INVALID_FILE_ATTRIBUTES; }

static void resolve_paths() {
    json cfg = json::object();
    { std::ifstream f(g_cfg.root + "\\config.json"); if (f) { try { cfg = json::parse(f); } catch (...) {} } }
    g_proj = g_cfg.root;
    g_spec_path = g_cfg.root + "\\panel.json";
    g_ipc_dir = g_cfg.root + "\\data";
}

static void start_engine() {
    dk::init(g_cfg.root + "\\data", g_spec_path);
}

static Item* find_item(const std::string& id) {
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) if (it.id == id) return &it;
    return nullptr;
}

static void set_status(const std::string& s, bool okv) { g_status = s; g_status_ok = okv; g_status_at = GetTickCount64(); }

// ------------------------------------------------------------------ requests
static int64_t next_seq() {
    if (!g_seq) g_seq = (int64_t)(time(nullptr) % 1000000000);
    if (g_link.seq_done >= g_seq) g_seq = g_link.seq_done;
    return ++g_seq;
}
// set requests for the same id replace the queued one (only the newest value matters)
static int64_t enqueue(json r, const std::string& coalesce_id = std::string()) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    const int64_t s = next_seq();
    r["seq"] = s;
    if (!coalesce_id.empty())
        for (auto q = g_queue.begin(); q != g_queue.end();)
            if (jstr(*q, "op") == "set" && jstr(*q, "id") == coalesce_id) q = g_queue.erase(q); else ++q;
    g_queue.push_back(std::move(r));
    while (g_queue.size() > 64) g_queue.pop_front();
    return s;
}
static void send_set(Item& it, const json& v) {
    { const ULONGLONG now = GetTickCount64();
      if (now - it.last_vlog > 5000 || v.is_boolean()) { it.last_vlog = now; log("[option] %s = %s", it.label.c_str(), v.dump().substr(0, 80).c_str()); } }
    it.last_seq = enqueue({ {"op", "set"}, {"id", it.id}, {"v", v} }, it.id);
    it.waiting = true; it.sent_at = GetTickCount64();
}
// drop-down: the chosen option's value goes out like a slider / input value (a number when it reads as one)
static void send_choice(Item& it, int k) {
    if (k < 0 || k >= (int)it.options.size()) return;
    it.choice = k;
    const std::string& v = it.options[k].second;
    if (it.free_text && v.empty()) return;   // the "pick or type" placeholder
    char* end = nullptr; const double d = v.empty() ? 0 : strtod(v.c_str(), &end);
    if (!v.empty() && end && *end == 0) send_set(it, d); else send_set(it, v);
}
// Drop-down with a search box: lists of more than 12 options get a "type to search" field at the top (focused when the
// list opens); every typed word must appear in the option's label. Enter takes the first match, or - free_text items -
// sends the typed words when nothing matches.
static bool has_words(const std::string& label, const char* q) {
    std::string l = label; for (auto& c : l) c = (char)tolower((unsigned char)c);
    std::string w; std::string qs(q); for (auto& c : qs) c = (char)tolower((unsigned char)c);
    size_t i = 0; while (i <= qs.size()) { const size_t j = qs.find(' ', i); w = qs.substr(i, j == std::string::npos ? std::string::npos : j - i);
        if (!w.empty() && l.find(w) == std::string::npos) return false; if (j == std::string::npos) break; i = j + 1; }
    return true;
}
static void choice_combo(Item& it, const char* cur, ImGuiComboFlags flags) {
    if (!ImGui::BeginCombo("##c", cur, flags)) return;
    const bool search = it.options.size() > 12 || it.free_text;
    bool enter = false;
    if (search) {
        if (ImGui::IsWindowAppearing()) { it.filter[0] = 0; ImGui::SetKeyboardFocusHere(); }
        ImGui::SetNextItemWidth(-1);
        enter = ImGui::InputTextWithHint("##flt", it.free_text ? "Type to search, Enter = use these words" : "Type to search...",
                                         it.filter, sizeof it.filter, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::Separator();
    }
    int first = -1, shown = 0;
    for (int k = 0; k < (int)it.options.size(); ++k) {
        if (search && it.filter[0] && !has_words(it.options[k].first, it.filter)) continue;
        if (first < 0) first = k; ++shown;
        const bool sel = k == it.choice;
        if (ImGui::Selectable((it.options[k].first + "##o" + std::to_string(k)).c_str(), sel)) { send_choice(it, k); it.filter[0] = 0; }
        if (sel && !it.filter[0]) ImGui::SetItemDefaultFocus();
    }
    if (search && shown == 0) ImGui::TextDisabled(it.free_text ? "No match - Enter sends what you typed" : "No match");
    if (enter) {
        if (first >= 0 && it.filter[0]) send_choice(it, first);
        else if (it.free_text && it.filter[0]) send_set(it, std::string(it.filter));
        it.filter[0] = 0; ImGui::CloseCurrentPopup();
    }
    ImGui::EndCombo();
}
// does a value (number / text / bool) stand for this option value?
static bool option_is(const std::string& opt, const json& v) {
    if (v.is_string()) return opt == v.get<std::string>();
    if (v.is_boolean()) { const bool b = v.get<bool>(); return opt == (b ? "true" : "false") || opt == (b ? "1" : "0"); }
    if (v.is_number()) {
        char* end = nullptr; const double d = opt.empty() ? 0 : strtod(opt.c_str(), &end); const double x = v.get<double>();
        return !opt.empty() && end && *end == 0 && std::fabs(d - x) <= 1e-9 * std::max(1.0, std::fabs(x));
    }
    return false;
}
static bool send_click(Item& it, bool confirmed) {
    if (!lua_online()) {
        it.result = "not sent - " LINK_OFF; it.result_ok = false; it.result_at = GetTickCount64(); it.waiting = false;
        return false;
    }
    log("[option] %s (%s)", it.label.c_str(), it.button.c_str());
    it.last_seq = enqueue({ {"op", "click"}, {"id", it.id}, {"confirmed", confirmed} });
    it.waiting = true; it.sent_at = GetTickCount64();
    return true;
}

static void write_requests() {
    std::vector<json> batch;
    { std::lock_guard<std::recursive_mutex> lk(g_mtx); if (g_queue.empty()) return; batch.assign(g_queue.begin(), g_queue.end()); }
    if (dk::native_mode()) {                               // in-process engine: no files
        dk::post(json(batch));                              // runs each request on the game thread: can take a while
        std::set<int64_t> sent;
        for (auto& r : batch) sent.insert((int64_t)jnum(r, "seq", 0));
        std::lock_guard<std::recursive_mutex> lk(g_mtx);    // requests queued meanwhile stay for the next round
        for (auto q = g_queue.begin(); q != g_queue.end();)
            if (sent.count((int64_t)jnum(*q, "seq", 0))) q = g_queue.erase(q); else ++q;
        ++g_written;
        return;
    }
}

// worker thread: read state.json when it changed
static void apply_state(const json& j, const FILETIME& ft);
static void read_state() {
    if (dk::native_mode()) {                               // in-process engine: state document straight from memory
        static uint64_t last_v = 0;
        if (!ue::ready()) return;                           // engine core not up yet = offline
        FILETIME now; GetSystemTimeAsFileTime(&now);
        const uint64_t v = dk::state_version();
        { std::lock_guard<std::recursive_mutex> lk(g_mtx);
          const bool force = g_force_state; g_force_state = false;
          if (v == last_v && !force && g_link.ever) { g_link.mtime = now; return; } }
        last_v = v;
        apply_state(dk::state(), now);
        return;
    }
}
static void apply_state(const json& j, const FILETIME& ft) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    if (j.is_discarded() || !j.is_object()) { g_link.err = "state unreadable"; return; }
    g_link.ever = true; g_link.mtime = ft; g_link.err.clear(); ++g_link.reads;
    g_link.seq_done = (int64_t)jnum(j, "seq_done", (double)g_link.seq_done);
    g_link.version = jstr(j, "version", g_link.version);
    auto obj = [&](const char* k) { auto it = j.find(k); return (it != j.end() && it->is_object()) ? *it : json::object(); };
    g_link.keeper = obj("keeper"); g_link.player = obj("player"); g_link.selfheal = obj("selfheal");
    { auto it = j.find("panel_log"); g_link.panel_log = (it != j.end() && it->is_array()) ? *it : json::array(); }

    std::map<int64_t, std::pair<bool, std::string>> results;
    const json res_obj = obj("results");   // keep it alive: items() of a temporary dangles
    for (auto& kv : res_obj.items()) {
        const int64_t k = (int64_t)strtod(kv.key().c_str(), nullptr);
        const json& r = kv.value();
        if (r.is_object()) results[k] = { jbool(r, "ok", false), r.contains("res") ? as_text(r["res"]) : std::string() };
    }
    const json values = obj("values"), readouts = obj("readouts");
    const ULONGLONG now = GetTickCount64();
    for (auto& t : g_tabs) for (auto& sec : t.sections) for (auto& it : sec.items) {
        if (it.waiting) {
            auto r = results.find(it.last_seq);
            if (r != results.end()) {
                it.waiting = false; it.result_ok = r->second.first; it.result = r->second.second; it.result_at = now;
                set_status(it.label + ": " + (it.result.empty() ? (it.result_ok ? "ok" : "failed") : it.result), it.result_ok);
            } else if (g_link.seq_done >= it.last_seq && now - it.sent_at > 3000) {
                it.waiting = false; it.result_ok = true; it.result = "done"; it.result_at = now;   // result already rotated out
            }
        }
        auto v = values.find(it.id);
        if (v != values.end() && !it.dirty && !it.waiting && g_drag_id != it.id) {
            if (it.type == "toggle") { it.on = v->is_boolean() ? v->get<bool>() : (v->is_number() && v->get<double>() != 0); it.have_value = true; }
            else if (it.type == "slider" && v->is_number()) { it.val = v->get<double>(); it.have_value = true; }
            else if (it.type == "choice" && (v->is_number() || v->is_string() || v->is_boolean())) {   // drop-down follows the trainer (e.g. difficulty preset)
                for (int k = 0; k < (int)it.options.size(); ++k) if (option_is(it.options[k].second, *v)) { it.choice = k; it.have_value = true; break; }
            }
        }
        auto ro = readouts.find(it.id);
        if (ro != readouts.end()) it.readout = as_text(*ro);
    }
    // editor calls: hand the raw result JSON to the callback
    std::vector<std::pair<CallCb, std::pair<bool, json>>> fire;
    for (auto c = g_calls.begin(); c != g_calls.end();) {
        auto r = res_obj.find(std::to_string(c->first));
        if (r != res_obj.end() && r->is_object()) {
            fire.push_back({ std::move(c->second.cb), { jbool(*r, "ok", false), r->contains("res") ? (*r)["res"] : json() } });
            c = g_calls.erase(c);
        } else if (g_link.seq_done >= c->first && now - c->second.sent_at > 3000) {   // done but rotated out of results
            fire.push_back({ std::move(c->second.cb), { false, json("result lost (more than 20 requests since) - refresh") } });
            c = g_calls.erase(c);
        } else ++c;
    }
    for (auto& f : fire) if (f.first) f.first(f.second.first, f.second.second);
}

static int64_t lua_call(const std::string& fn, json args, CallCb cb) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    if (!lua_online()) { if (cb) cb(false, json(LINK_OFF " - not sent")); return 0; }
    const int64_t s = enqueue({ {"op", "call"}, {"fn", fn}, {"args", std::move(args)} });
    g_calls[s] = PendingCall{ fn, GetTickCount64(), std::move(cb) };
    return s;
}
static bool call_busy(int64_t s) { return s != 0 && g_calls.count(s) != 0; }

static void worker() {
    ULONGLONG last_state = 0, last_spec = 0;
    log("[panel] panel %s, data %s", g_spec_path.c_str(), g_ipc_dir.c_str());
    while (!g_retired && !g_features_off) {
        try {
            write_requests();
            const ULONGLONG now = GetTickCount64();
            if (now - last_state >= 250) { last_state = now; read_state(); }
            {
                std::lock_guard<std::recursive_mutex> lk(g_mtx);
                for (auto& t : g_tabs) for (auto& sec : t.sections) for (auto& it : sec.items)
                    if (it.waiting && now - it.sent_at > 8000) {
                        it.waiting = false; it.result_ok = false; it.result_at = now;
                        it.result = lua_online() ? "no answer yet (still queued?)" : "queued - " LINK_OFF;
                    }
                std::vector<CallCb> late;
                for (auto c = g_calls.begin(); c != g_calls.end();) {
                    if (now - c->second.sent_at > kCallTimeoutMs) {
                        const int64_t sq = c->first;   // still unsent? drop it so it never fires late
                        for (auto q = g_queue.begin(); q != g_queue.end();) if ((int64_t)jnum(*q, "seq", 0) == sq) q = g_queue.erase(q); else ++q;
                        late.push_back(std::move(c->second.cb)); c = g_calls.erase(c);
                    } else ++c;
                }
                for (auto& cb : late) if (cb) cb(false, json(lua_online() ? "timed out after 8 s (no answer)" : "timed out after 8 s - " LINK_OFF));
            }
        } catch (std::exception& e) { log("[panel] worker: %s", e.what()); Sleep(500); }
        Sleep(50);
    }
    g_worker_on = false;
}

// ------------------------------------------------------------------ spec load
json panel_reload() {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    if (g_spec_path.empty()) resolve_paths();
    file_mtime(g_spec_path, g_spec_mtime);        // also on failure: retry only when the file changes again
    std::ifstream f(g_spec_path, std::ios::binary);
    if (!f) { g_load_msg = "panel spec not found: " + g_spec_path; log("[panel] %s", g_load_msg.c_str()); return fail("NO_SPEC", g_load_msg); }
    json j;
    try { j = json::parse(f); }
    catch (std::exception& e) { g_load_msg = std::string("panel.json error (kept the previous one): ") + e.what(); log("[panel] %s", g_load_msg.c_str()); return fail("BAD_PANEL", g_load_msg); }
    if (!j.is_object()) { g_load_msg = "panel.json is not an object"; return fail("BAD_PANEL", g_load_msg); }

    std::map<std::string, Item> old;
    for (auto& t : g_tabs) for (auto& s : t.sections) for (auto& it : s.items) old[it.id] = it;

    std::vector<Tab> tabs; int count = 0;
    g_title = jstr(j, "title", "BORDERLANDS 4 TRAINER");
    auto jt_arr = j.find("tabs");
    if (jt_arr != j.end() && jt_arr->is_array()) for (auto& jt : *jt_arr) {
        if (!jt.is_object()) continue;
        if (WB_PUBLIC && jbool(jt, "dev", false)) continue;
        Tab t; t.name = jstr(jt, "name", "Tab"); t.page = jstr(jt, "page");
        auto js_arr = jt.find("sections");
        if (js_arr != jt.end() && js_arr->is_array()) for (auto& js : *js_arr) {
            if (!js.is_object()) continue;
            if (WB_PUBLIC && jbool(js, "dev", false)) continue;
            Section s; s.name = jstr(js, "name");
            auto ji_arr = js.find("items");
            if (ji_arr != js.end() && ji_arr->is_array()) for (auto& ji : *ji_arr) {
                if (!ji.is_object()) continue;
                if (WB_PUBLIC && jbool(ji, "dev", false)) continue;
                Item it;
                it.id = jstr(ji, "id", t.name + "_" + std::to_string(count));
                it.type = jstr(ji, "type", "label");
                it.text = jstr(ji, "text");
                it.label = jstr(ji, "label", it.text.empty() ? it.id : it.text);
                it.desc = jstr(ji, "desc");
                it.button = jstr(ji, "button", "RUN");
                it.hotkey = jstr(ji, "hotkey");
                it.danger = jbool(ji, "danger", false);
                it.minv = jnum(ji, "min", 0); it.maxv = jnum(ji, "max", it.minv + 100); it.step = jnum(ji, "step", 0);
                if (it.maxv <= it.minv) it.maxv = it.minv + 1;
                it.defv = jnum(ji, "default", it.minv);
                it.has_off = ji.contains("off") && ji["off"].is_number();
                it.offv = jnum(ji, "off", it.defv);
                it.val = it.defv;
                it.free_text = ji.contains("free_text") && ji["free_text"].is_boolean() && ji["free_text"].get<bool>();
                { auto jo = ji.find("options");   // choice items: ["text", ...] or [{label, value}, ...]
                  if (jo != ji.end() && jo->is_array()) for (auto& op : *jo) {
                      if (op.is_string()) it.options.push_back({ op.get<std::string>(), op.get<std::string>() });
                      else if (op.is_object()) it.options.push_back({ jstr(op, "label", jstr(op, "value")), op.contains("value") && !op["value"].is_string() ? op["value"].dump() : jstr(op, "value") });
                  } }
                auto o = old.find(it.id);
                if (o != old.end()) {
                    const Item& p = o->second;
                    it.on = p.on; it.val = p.val; it.have_value = p.have_value; it.dirty = p.dirty; it.last_edit = p.last_edit;
                    it.last_seq = p.last_seq; it.sent_at = p.sent_at; it.waiting = p.waiting;
                    it.result = p.result; it.result_ok = p.result_ok; it.result_at = p.result_at; it.readout = p.readout;
                    memcpy(it.input, p.input, sizeof it.input);
                    it.choice = p.choice < (int)it.options.size() ? p.choice : 0;
                }
                s.items.push_back(std::move(it)); ++count;
            }
            t.sections.push_back(std::move(s));
        }
        tabs.push_back(std::move(t));
    }
    const int old_n = (int)g_tabs.size();
    g_tabs = std::move(tabs);
    {   const int new_n = (int)g_tabs.size();
        if (g_tab > old_n) g_tab = new_n + (g_tab - old_n);
        else if (g_tab < 0 || g_tab >= new_n) g_tab = 0; }   // panel_draw falls back to the first shown page
    g_force_state = true;        // re-read state.json now so new items get their values
    g_load_msg = "Loaded " + std::to_string(count) + " items in " + std::to_string(g_tabs.size()) + " tabs";
    log("[panel] %s from %s", g_load_msg.c_str(), g_spec_path.c_str());
    return ok({ {"items", count}, {"tabs", g_tabs.size()}, {"spec", g_spec_path} }, g_load_msg);
}


void panel_init() {
    resolve_paths();
    start_engine();
    panel_reload();
    if (!g_worker_on.exchange(true)) std::thread(worker).detach();
}

// ------------------------------------------------------------------ widgets
static bool toggle_switch(const char* id, bool* v) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    float h = ImGui::GetFrameHeight() * 0.78f, w = h * 1.85f;
    ImVec2 p = ImGui::GetCursorScreenPos();
    p.y += (ImGui::GetFrameHeight() - h) * 0.5f;
    ImGui::InvisibleButton(id, ImVec2(w, ImGui::GetFrameHeight()));
    bool clicked = ImGui::IsItemClicked();
    if (clicked) *v = !*v;
    ImGuiStorage* st = ImGui::GetStateStorage();
    ImGuiID sid = ImGui::GetID(id);
    float t = st->GetFloat(sid, *v ? 1.f : 0.f);
    t += (*v ? 1.f : -1.f) * ImGui::GetIO().DeltaTime * 9.f; t = ImClamp(t, 0.f, 1.f);
    st->SetFloat(sid, t);
    ImU32 off = ui::rgb(0x2A2826), on = ui::rgb(ui::kGold);
    ImVec4 a = ImGui::ColorConvertU32ToFloat4(off), b = ImGui::ColorConvertU32ToFloat4(on);
    ImU32 col = ImGui::GetColorU32(ImLerp(a, b, t));
    if (ImGui::IsItemHovered()) col = ImGui::GetColorU32(ImLerp(ImLerp(a, b, t), ImVec4(1, 1, 1, 1), 0.08f));
    dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), col, h * 0.5f);
    dl->AddRect(p, ImVec2(p.x + w, p.y + h), ui::rgb(ui::kGold, .35f + .4f * t), h * 0.5f);
    float r = h * 0.5f - 2.5f;
    dl->AddCircleFilled(ImVec2(p.x + r + 2.5f + t * (w - 2 * r - 5), p.y + h * 0.5f), r, ui::rgb(t > .5f ? ui::kInk : ui::kBone));
    return clicked;
}

namespace sk { static bool on(); static void section(const char* name); }
static void section_header(const char* name) {
    if (sk::on()) { sk::section(name); return; }
    ImGui::Dummy(ImVec2(0, 4));
    ImGui::PushFont(g_font_bold);
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(ui::kGoldHi));
    std::string up = name; std::transform(up.begin(), up.end(), up.begin(), ::toupper);
    ImGui::TextUnformatted(up.c_str());
    ImGui::PopStyleColor(); ImGui::PopFont();
    ImVec2 p = ImGui::GetCursorScreenPos();
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x, p.y - 3), ImVec2(p.x + std::min(S(120.f), ImGui::GetContentRegionAvail().x), p.y - 3), ui::rgb(ui::kGold), S(1.5f));
    ImGui::GetWindowDrawList()->AddLine(ImVec2(p.x + std::min(S(120.f), ImGui::GetContentRegionAvail().x), p.y - 3), ImVec2(p.x + ImGui::GetContentRegionAvail().x, p.y - 3), ui::rgb(ui::kGold, .22f));
    ImGui::Dummy(ImVec2(0, 2));
}

static bool matches(const Item& it) {
    if (!g_search[0]) return true;
    std::string a = it.label + " " + it.desc + " " + it.id, b = g_search;
    std::transform(a.begin(), a.end(), a.begin(), ::tolower); std::transform(b.begin(), b.end(), b.begin(), ::tolower);
    return a.find(b) != std::string::npos;
}

static std::string slider_fmt(double step) {
    int d = 2;
    if (step > 0) { d = 0; double s = step; while (d < 4 && std::fabs(s - std::round(s)) > 1e-6) { s *= 10; ++d; } }
    char f[16]; sprintf_s(f, "%%.%df", d); return f;
}

static void text_dim(const std::string& s, uint32_t col = ui::kDim) {
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(col));
    ImGui::PushTextWrapPos(ImGui::GetWindowContentRegionMax().x - S(10));
    ImGui::TextUnformatted(s.c_str());
    ImGui::PopTextWrapPos(); ImGui::PopStyleColor();
}


// Drawn through trinity::ui::DrawItemIcon (skin.cpp). Without a skin folder the classic look below is used unchanged.
extern ImFont* g_font_skin;
extern ImFont* g_font_skin_big;
namespace sk {
struct Piece { std::string kind; float w = 0, h = 0, l = 0, t = 0, r = 0, b = 0; };
static bool g_loaded = false, g_ok = false, g_classic = false;
static std::map<std::string, Piece> g_p;
static json g_lay = json::object();
static float K = 0.65f;                                  // skin px -> screen px (set per frame from the panel scale)
constexpr uint32_t kYellow = 0xFFC81E, kYellowHi = 0xFFE066, kWhite = 0xEEEEE8, kInkT = 0x17171A, kGrey = 0x9A9A9A;
static bool load() {
    g_loaded = true; g_ok = false;
    try {
        std::ifstream f(g_cfg.root + "\\skin\\skin.json");
        if (!f) return false;
        json j = json::parse(f, nullptr, false);
        if (!j.is_object() || !j.contains("pieces")) return false;
        for (auto& [n, v] : j["pieces"].items()) {
            Piece p; p.kind = jstr(v, "kind"); p.w = (float)jnum(v, "w", 0); p.h = (float)jnum(v, "h", 0);
            p.l = (float)jnum(v, "l", 0); p.t = (float)jnum(v, "t", 0); p.r = (float)jnum(v, "r", 0); p.b = (float)jnum(v, "b", 0);
            g_p[n] = p;
        }
        if (j.contains("layout") && j["layout"].is_object()) g_lay = j["layout"];
        g_ok = g_p.count("frame") && g_p.count("row");
    } catch (...) { g_ok = false; }
    return g_ok;
}
static bool on() { if (!g_loaded) load(); return g_ok && !g_classic; }
static float L(const char* k, float d = 0) { return (float)jnum(g_lay, k, d) * K; }
static ImVec2 size(const char* n) { auto it = g_p.find(n); return it == g_p.end() ? ImVec2(0, 0) : ImVec2(it->second.w * K, it->second.h * K); }
static bool draw(ImDrawList* dl, const char* n, ImVec2 a, ImVec2 b, ImU32 tint = IM_COL32_WHITE) {
    auto it = g_p.find(n); if (it == g_p.end()) return false;
    const Piece& p = it->second; char spec[160];
    if (p.kind == "3slice") _snprintf_s(spec, sizeof spec, _TRUNCATE, "%s#3:%g,%g", n, p.l, p.r);
    else if (p.kind == "9slice") _snprintf_s(spec, sizeof spec, _TRUNCATE, "%s#9:%g,%g,%g,%g@%g", n, p.l, p.t, p.r, p.b, K);
    else _snprintf_s(spec, sizeof spec, _TRUNCATE, "%s", n);
    return trinity::ui::DrawItemIcon(dl, spec, a, b, tint);
}
// text in a font, aligned in [a,b] (ax 0 left .5 centre 1 right), vertically centred
static void text(ImDrawList* dl, ImFont* f, ImVec2 a, ImVec2 b, ImU32 col, const char* s, float ax = .5f, bool shadow = true, float scale = 1.f) {
    ImGui::PushFont(f); const float fs = ImGui::GetFontSize() * scale; const ImVec2 ts(ImGui::CalcTextSize(s).x * scale, ImGui::CalcTextSize(s).y * scale); ImGui::PopFont();
    const ImVec2 p(a.x + (b.x - a.x - ts.x) * ax, a.y + (b.y - a.y - ts.y) * .5f);
    if (shadow) dl->AddText(f, fs, ImVec2(p.x + 1.5f, p.y + 1.5f), IM_COL32(0, 0, 0, 200), s);
    dl->AddText(f, fs, p, col, s);
}
static ImU32 rgb_(uint32_t c, float a = 1.f) { return IM_COL32((c >> 16) & 255, (c >> 8) & 255, c & 255, (int)(a * 255)); }
static std::string upper(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::toupper); return s; }
// a plate button (button / tab / badge pieces): returns true when clicked
static bool plate_button(const char* id, const char* piece, const char* label, ImVec2 sz, ImU32 txt, ImFont* f) {
    const ImVec2 p = ImGui::GetCursorScreenPos();
    sz.x = std::max(sz.x, 1.f); sz.y = std::max(sz.y, 1.f);   // a missing skin piece has no size: never a zero-size item
    const bool click = ImGui::InvisibleButton(id, sz);
    const bool hov = ImGui::IsItemHovered(), act = ImGui::IsItemActive();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImU32 tint = act ? IM_COL32(200, 200, 200, 255) : hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(232, 232, 232, 255);
    draw(dl, piece, p, ImVec2(p.x + sz.x, p.y + sz.y), tint);
    text(dl, f, p, ImVec2(p.x + sz.x, p.y + sz.y), txt, label, .5f, false);
    return click;
}
} // namespace sk

static void draw_item_skin(Item& it);
static void draw_item(Item& it) {
    const ULONGLONG now = GetTickCount64();
    const bool online = lua_online();
    ImGui::PushID(it.id.c_str());

    if (it.type == "label" || it.type == "note" || it.type == "text" || it.type == "info") {   // plain text rows
        ImGui::Dummy(ImVec2(0, S(2)));
        text_dim(it.text.empty() ? it.label : it.text, ui::kBone);
        if (!it.desc.empty()) text_dim(it.desc);
        ImGui::Dummy(ImVec2(0, S(4)));
        ImGui::PopID();
        return;
    }
    if (sk::on()) { draw_item_skin(it); ImGui::PopID(); return; }

    ImDrawList* row_dl = ImGui::GetWindowDrawList(); row_dl->ChannelsSplit(2); row_dl->ChannelsSetCurrent(1);
    const ImVec2 row_p0 = ImGui::GetCursorScreenPos(); ImGui::Dummy(ImVec2(0, S(1)));
    const float avail = ImGui::GetContentRegionAvail().x;
    const float right = ImGui::GetWindowContentRegionMax().x;
    const bool known = it.type == "toggle" || it.type == "slider" || it.type == "button" || it.type == "readout" || it.type == "input" || it.type == "choice";
    float ctrl_w = it.type == "toggle" ? ImGui::GetFrameHeight() * 0.78f * 1.85f
                 : it.type == "button" ? S(130.f)
                 : it.type == "slider" || it.type == "input" || it.type == "choice" ? std::min(S(320.f), avail * 0.5f) : 0.f;
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(it.label.c_str());
    if (!it.desc.empty() && ImGui::IsItemHovered()) ImGui::SetTooltip("%s", it.desc.c_str());
    bool active_mark = false;

    if (it.type == "choice") {
        ImGui::SameLine(right - ctrl_w);
        ImGui::SetNextItemWidth(ctrl_w);
        const char* cur = it.options.empty() ? "-" : it.options[std::clamp(it.choice, 0, (int)it.options.size() - 1)].first.c_str();
        choice_combo(it, cur, ImGuiComboFlags_HeightLarge);
    } else if (it.type == "toggle") {
        ImGui::SameLine(right - ctrl_w);
        bool v = it.on;
        if (toggle_switch("##sw", &v)) { it.on = v; send_set(it, v); }
        active_mark = it.on;
    } else if (it.type == "slider") {
        ImGui::SameLine(right - ctrl_w);
        ImGui::SetNextItemWidth(ctrl_w);
        double v = it.val, mn = it.minv, mx = it.maxv;
        const std::string fmt = slider_fmt(it.step);
        if (ImGui::SliderScalar("##v", ImGuiDataType_Double, &v, &mn, &mx, fmt.c_str())) {
            if (it.step > 0) v = mn + std::round((v - mn) / it.step) * it.step;
            it.val = std::clamp(v, mn, mx); it.dirty = true; it.last_edit = now;
        }
        const bool act = ImGui::IsItemActive();
        if (act) g_drag_id = it.id;
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { it.val = it.has_off ? it.offv : it.defv; it.dirty = true; it.last_edit = 0; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%sRight-click: back to %g%s", it.desc.c_str(), it.desc.empty() ? "" : "\n", it.has_off ? it.offv : it.defv, it.has_off ? " (off)" : "");
        if (it.dirty && (!act || now - it.last_edit >= 250)) { send_set(it, it.val); it.dirty = false; }
        active_mark = it.has_off && std::fabs(it.val - it.offv) > 1e-9;
    } else if (it.type == "button") {
        ImGui::SameLine(right - ctrl_w);
        const bool arming = it.danger && now < it.confirm_until;
        const std::string cap = (arming ? std::string("CONFIRM?") : it.button) + "###b";
        const bool pressed = it.danger ? ui::danger(cap.c_str(), ctrl_w, ImGui::GetFrameHeight())
                                       : ui::primary(cap.c_str(), ctrl_w, ImGui::GetFrameHeight());
        if (it.danger && ImGui::IsItemHovered()) ImGui::SetTooltip(arming ? "Click again to confirm" : "Needs a second click within 4 s");
        if (pressed) {
            if (!it.danger) send_click(it, false);
            else if (arming) { it.confirm_until = 0; send_click(it, true); }
            else it.confirm_until = now + 4000;
        }
    } else if (it.type == "readout") {
        const std::string v = it.readout.empty() ? (online ? "-" : "--") : it.readout;
        const float w = ImGui::CalcTextSize(v.c_str()).x;
        ImGui::SameLine(std::max(ImGui::GetItemRectMax().x - ImGui::GetWindowPos().x + S(12), right - w - S(4)));
        ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(online ? ui::kGoldHi : ui::kDim));
        ImGui::TextUnformatted(v.c_str());
        ImGui::PopStyleColor();
    } else if (it.type == "input") {
        ImGui::SameLine(right - ctrl_w);
        ImGui::SetNextItemWidth(ctrl_w - S(62));
        const bool enter = ImGui::InputText("##t", it.input, sizeof it.input, ImGuiInputTextFlags_EnterReturnsTrue);
        ImGui::SameLine();
        if (ui::primary("SET##go", S(54), ImGui::GetFrameHeight()) || enter) send_set(it, std::string(it.input));
    } else if (!known) {
        ImGui::SameLine();
        ImGui::TextDisabled("(%s)", it.type.c_str());
    }

    if (!it.desc.empty()) text_dim(it.desc);
    // drop-downs and sliders show their value already: only failures get a line (a "> done" line would shift the rows
    // below while the user is still clicking down the page)
    const bool quiet_ok = it.type == "choice" || it.type == "slider";
    if (it.waiting && !quiet_ok) text_dim(online ? "sending..." : "queued - " LINK_OFF, ui::kAmber);
    else if (it.result_at && now - it.result_at < 10000 && !(quiet_ok && it.result_ok)) text_dim(std::string(it.result_ok ? "> " : "! ") + it.result, it.result_ok ? ui::kGreen : ui::kRed);

    ImGui::Dummy(ImVec2(0, S(1)));
    {   const ImVec2 row_p1(ImGui::GetWindowPos().x + right + S(5), ImGui::GetCursorScreenPos().y - S(3));
        row_dl->ChannelsSetCurrent(0);
        const bool hov = ImGui::IsMouseHoveringRect(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), row_p1);
        row_dl->AddRectFilledMultiColor(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), row_p1, ui::rgb(hov ? 0x22231F : 0x16171A, .92f), ui::rgb(hov ? 0x1A1B1E : 0x101114, .80f), ui::rgb(hov ? 0x1A1B1E : 0x101114, .80f), ui::rgb(hov ? 0x22231F : 0x16171A, .92f));
        row_dl->AddRect(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), row_p1, ui::rgb(hov ? ui::kGold : ui::kLine, hov ? .45f : .7f));
        if (active_mark) row_dl->AddRectFilled(ImVec2(row_p0.x - S(7), row_p0.y - S(4)), ImVec2(row_p0.x - S(4), row_p1.y), ui::rgb(ui::kGold));
        row_dl->ChannelsMerge(); }
    ImGui::Dummy(ImVec2(0, S(2)));
    ImGui::PopID();
}


// ---- skinned rows: a row plate per item, label left, control right, hotkey badge at the far right
namespace sk {
static void section(const char* name) {
    ImGui::Dummy(ImVec2(0, 6 * K));
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 p = ImGui::GetCursorScreenPos(); const float w = ImGui::GetContentRegionAvail().x;
    const std::string up = upper(name);
    ImGui::PushFont(g_font_skin); const ImVec2 ts = ImGui::CalcTextSize(up.c_str()); ImGui::PopFont();
    text(dl, g_font_skin, p, ImVec2(p.x + ts.x, p.y + ts.y), rgb_(kYellow), up.c_str(), 0.f);
    const float y = p.y + ts.y * .55f;
    dl->AddLine(ImVec2(p.x + ts.x + 12 * K, y), ImVec2(p.x + w - 6 * K, y), rgb_(kYellow, .45f), 2.f * K);
    ImGui::Dummy(ImVec2(w, ts.y + 6 * K));
}
} // namespace sk

static void draw_item_skin(Item& it) {
    using namespace sk;
    const ULONGLONG now = GetTickCount64();
    const bool online = lua_online();
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float rowh = size("row").y;
    const ImVec2 p0 = ImGui::GetCursorScreenPos();
    const float w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p1(p0.x + w, p0.y + rowh);
    const bool hov = ImGui::IsMouseHoveringRect(p0, p1) && ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByActiveItem);
    draw(dl, "row", p0, p1, hov ? IM_COL32(255, 255, 255, 255) : IM_COL32(222, 222, 222, 255));
    const float pad = 20 * K;
    // badge column (always reserved for toggles so the check boxes line up)
    const ImVec2 bsz(size("badge").x * (jstr(g_lay, "badge_prefix").empty() ? .8f : 1.f), size("badge").y);
    const float bx = p1.x - pad - bsz.x;
    if (!it.hotkey.empty()) {
        const ImVec2 a(bx, p0.y + (rowh - bsz.y) * .5f), b(a.x + bsz.x, a.y + bsz.y);
        draw(dl, "badge", a, b);
        const std::string pre = jstr(g_lay, "badge_prefix");
        if (pre.empty() || it.hotkey.size() > 4) text(dl, g_font_skin, a, b, rgb_(kYellow), upper(it.hotkey).c_str());
        else {
            const float half = a.x + (b.x - a.x) * .52f;
            text(dl, g_font_skin, ImVec2(a.x + 12 * K, a.y), ImVec2(half, b.y), rgb_(kWhite), pre.c_str(), .5f, true, .8f);
            text(dl, g_font_skin, ImVec2(half, a.y), ImVec2(b.x - 10 * K, b.y), rgb_(kYellow), upper(it.hotkey).c_str(), .5f);
        }
    }
    const float cr = it.hotkey.empty() && it.type != "toggle" ? p1.x - pad : bx - 14 * K;   // right edge for the control
    float ctrl_l = cr;
    const float fh = ImGui::GetFrameHeight();
    const float cy = p0.y + (rowh - fh) * .5f;
    bool lit = false;

    if (it.type == "toggle") {
        ImGui::SetCursorScreenPos(p0);
        if (ImGui::InvisibleButton("##row", ImVec2(w, rowh))) { it.on = !it.on; send_set(it, it.on); }
        const ImVec2 cs(size("check_on").x * .9f, size("check_on").y * .9f);
        const ImVec2 a(cr - cs.x, p0.y + (rowh - cs.y) * .5f);
        draw(dl, it.on ? "check_on" : "check_off", a, ImVec2(a.x + cs.x, a.y + cs.y));
        ctrl_l = a.x; lit = it.on;
    } else if (it.type == "button") {
        const bool arming = it.danger && now < it.confirm_until;
        const std::string cap = upper(arming ? std::string("CONFIRM?") : it.button);
        ImGui::PushFont(g_font_skin); const float tw = ImGui::CalcTextSize(cap.c_str()).x; ImGui::PopFont();
        const ImVec2 bs(std::max(150 * K, tw + 48 * K), size("button").y * .95f);
        ctrl_l = cr - bs.x;
        ImGui::SetCursorScreenPos(ImVec2(ctrl_l, p0.y + (rowh - bs.y) * .5f));
        const bool pressed = plate_button("##b", "button", cap.c_str(), bs, rgb_(arming ? 0xA01010 : kInkT), g_font_skin);
        if (it.danger && ImGui::IsItemHovered()) ImGui::SetTooltip(arming ? "Click again to confirm" : "Needs a second click within 4 s");
        if (pressed) {
            if (!it.danger) send_click(it, false);
            else if (arming) { it.confirm_until = 0; send_click(it, true); }
            else it.confirm_until = now + 4000;
        }
    } else if (it.type == "readout") {
        const std::string v = it.readout.empty() ? (online ? "-" : "--") : it.readout;
        ImGui::PushFont(g_font_skin); const float tw = ImGui::CalcTextSize(v.c_str()).x; ImGui::PopFont();
        const ImVec2 fs(std::min(std::max(160 * K, tw + 50 * K), w * .6f), size("field").y * .85f);
        const ImVec2 a(cr - fs.x, p0.y + (rowh - fs.y) * .5f), b(cr, a.y + fs.y);
        draw(dl, "badge", a, b);
        dl->PushClipRect(a, b, true);
        text(dl, g_font_skin, ImVec2(a.x + 14 * K, a.y), ImVec2(b.x - 14 * K, b.y), rgb_(online ? kYellow : kGrey), v.c_str(), tw + 28 * K > fs.x ? 0.f : .5f);
        dl->PopClipRect();
        ctrl_l = a.x;
    } else if (it.type == "slider" || it.type == "input" || it.type == "choice") {
        const float go = it.type == "input" ? 84 * K : 0.f;
        const float cw = std::min(420 * K, w * .48f);
        const ImVec2 fs(cw, size("field").y * .9f);
        const ImVec2 a(cr - go - fs.x, p0.y + (rowh - fs.y) * .5f), b(a.x + fs.x, a.y + fs.y);
        // the field piece has a drop arrow in its right cap: only choices keep it, the rest use the plain badge plate
        draw(dl, it.type == "choice" ? "field" : "badge", a, b);
        ctrl_l = a.x;
        ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(0, 0, 0, 0)); ImGui::PushStyleColor(ImGuiCol_FrameBgHovered, IM_COL32(255, 255, 255, 14));
        ImGui::PushStyleColor(ImGuiCol_FrameBgActive, IM_COL32(255, 255, 255, 22));
        ImGui::PushStyleColor(ImGuiCol_Button, IM_COL32(0, 0, 0, 0)); ImGui::PushStyleColor(ImGuiCol_ButtonHovered, IM_COL32(0, 0, 0, 0));
        ImGui::PushStyleColor(ImGuiCol_SliderGrab, rgb_(kYellow)); ImGui::PushStyleColor(ImGuiCol_SliderGrabActive, rgb_(kYellowHi));
        ImGui::PushStyleColor(ImGuiCol_Text, rgb_(kWhite)); ImGui::PushStyleVar(ImGuiStyleVar_FrameBorderSize, 0.f);
        const float inset = 12 * K, right_cap = it.type == "choice" ? 40 * K : 12 * K;
        ImGui::SetCursorScreenPos(ImVec2(a.x + inset, cy));
        ImGui::SetNextItemWidth(fs.x - inset - right_cap);
        if (it.type == "choice") {
            const char* cur = it.options.empty() ? "-" : it.options[std::clamp(it.choice, 0, (int)it.options.size() - 1)].first.c_str();
            choice_combo(it, cur, ImGuiComboFlags_HeightLarge | ImGuiComboFlags_NoArrowButton);
        } else if (it.type == "slider") {
            double v = it.val, mn = it.minv, mx = it.maxv;
            const std::string fmt = slider_fmt(it.step);
            if (ImGui::SliderScalar("##v", ImGuiDataType_Double, &v, &mn, &mx, fmt.c_str())) {
                if (it.step > 0) v = mn + std::round((v - mn) / it.step) * it.step;
                it.val = std::clamp(v, mn, mx); it.dirty = true; it.last_edit = now;
            }
            const bool act = ImGui::IsItemActive();
            if (act) g_drag_id = it.id;
            if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { it.val = it.has_off ? it.offv : it.defv; it.dirty = true; it.last_edit = 0; }
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%sRight-click: back to %g%s", it.desc.c_str(), it.desc.empty() ? "" : "\n", it.has_off ? it.offv : it.defv, it.has_off ? " (off)" : "");
            if (it.dirty && (!act || now - it.last_edit >= 250)) { send_set(it, it.val); it.dirty = false; }
            lit = it.has_off && std::fabs(it.val - it.offv) > 1e-9;
        } else {
            const bool enter = ImGui::InputText("##t", it.input, sizeof it.input, ImGuiInputTextFlags_EnterReturnsTrue);
            ImGui::SetCursorScreenPos(ImVec2(b.x + 8 * K, p0.y + (rowh - size("button").y * .95f) * .5f));
            if (plate_button("##go", "button", "GO", ImVec2(go - 8 * K, size("button").y * .95f), rgb_(kInkT), g_font_skin) || enter) send_set(it, std::string(it.input));
        }
        ImGui::PopStyleVar(); ImGui::PopStyleColor(8);
    }

    // label (+ live state line) on the left
    {   const ImVec2 la(p0.x + 26 * K, p0.y), lb(ctrl_l - 12 * K, p1.y);
        std::string label = it.label;
        if (!it.hotkey.empty()) { const size_t br = label.find('['); if (br != std::string::npos) { label.resize(br); while (!label.empty() && label.back() == ' ') label.pop_back(); } }
        std::string sub;
        if (it.waiting && it.type != "choice" && it.type != "slider") sub = online ? "sending..." : "queued";
        else if (it.result_at && now - it.result_at < 8000 && !it.result_ok) sub = "! " + it.result;
        else if (it.result_at && now - it.result_at < 4000 && it.type == "button") sub = "> " + it.result;
        dl->PushClipRect(la, lb, true);
        if (sub.empty()) text(dl, g_font_skin, la, lb, rgb_(lit ? kYellow : kWhite), (jnum(g_lay, "row_upper", 1) != 0 ? upper(label) : label).c_str(), 0.f);
        else {
            const float mid = p0.y + rowh * .5f;
            text(dl, g_font_skin, ImVec2(la.x, p0.y + 4 * K), ImVec2(lb.x, mid + 2 * K), rgb_(lit ? kYellow : kWhite), (jnum(g_lay, "row_upper", 1) != 0 ? upper(label) : label).c_str(), 0.f);
            text(dl, g_font_body, ImVec2(la.x, mid), ImVec2(lb.x, p1.y - 6 * K), rgb_(it.result_ok ? 0x9FD86A : 0xFF6A5A), sub.c_str(), 0.f);
        }
        dl->PopClipRect();
        if (!it.desc.empty() && ImGui::IsMouseHoveringRect(la, lb) && it.type != "slider") ImGui::SetTooltip("%s", it.desc.c_str()); }

    ImGui::SetCursorScreenPos(ImVec2(p0.x, p1.y + 8 * K));
    ImGui::Dummy(ImVec2(0, 0));
}

static void kv_row(const char* k, const std::string& v) {
    ImGui::TextUnformatted(k); ImGui::SameLine(S(190)); ImGui::TextDisabled("%s", v.c_str());
}


// ------------------------------------------------------------------ SETTINGS page (2026-11-09)
// Hotkey capture: "Change..." records which keys are down, then each frame looks for a key that goes down (keyboard
// only: GetAsyncKeyState 0x08..0xFE minus mouse buttons / bare modifiers / Windows keys). Esc cancels, 10 s timeout.
// Ctrl / Shift / Alt held at that moment become part of the hotkey. While waiting, the overlay ignores the panel
// toggle (overlay_hotkey_capture_ping), so pressing the current hotkey here does not close the panel.
static bool g_capturing = false;
static ULONGLONG g_cap_until = 0;
static bool g_cap_prev[256] = {};
static std::string g_set_msg;
static bool g_set_ok = true;
static ULONGLONG g_set_at = 0;
static float g_scale_edit = -1.f;      // slider value while dragging (applied on release, so the slider does not move under the mouse)

static void settings_note(const std::string& m, bool okv) { g_set_msg = m; g_set_ok = okv; g_set_at = GetTickCount64(); }
static void settings_commit(const std::string& what) {
    std::string err;
    if (settings_save(&err)) settings_note(what, true);
    else settings_note("Could not save settings.json: " + err, false);
}
static bool game_focused() { DWORD pid = 0; GetWindowThreadProcessId(GetForegroundWindow(), &pid); return pid == GetCurrentProcessId(); }
static bool key_held(int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; }

static void capture_begin() {
    g_capturing = true; g_cap_until = GetTickCount64() + 10000;
    for (int vk = 0; vk < 256; ++vk) g_cap_prev[vk] = key_held(vk);
    overlay_hotkey_capture_ping();
}
static void capture_cancel(const char* why) { if (!g_capturing) return; g_capturing = false; settings_note(why, false); }

static void capture_poll() {
    if (!g_capturing) return;
    overlay_hotkey_capture_ping();
    if (GetTickCount64() > g_cap_until) { capture_cancel("No key pressed in 10 s - hotkey unchanged"); return; }
    const bool focus = game_focused();
    int got = 0;
    for (int vk = 1; vk < 256; ++vk) {
        const bool d = key_held(vk);
        const bool rose = d && !g_cap_prev[vk];
        g_cap_prev[vk] = d;
        if (!rose || !focus || got) continue;
        if (vk == VK_ESCAPE) { got = -1; continue; }
        if (hotkey_vk_allowed(vk)) got = vk;
    }
    if (got < 0) { capture_cancel("Cancelled - hotkey unchanged"); return; }
    if (!got) return;
    Hotkey h; h.vk = got; h.ctrl = key_held(VK_CONTROL); h.shift = key_held(VK_SHIFT); h.alt = key_held(VK_MENU);
    g_capturing = false;
    g_settings.hotkey = h;
    settings_commit("Panel hotkey is now " + hotkey_name(h));
}

static void draw_settings_tab() {
    const float bh = ImGui::GetFrameHeight() + S(4);
    section_header("Panel hotkey");
    kv_row("Keyboard", hotkey_name(g_settings.hotkey));
    if (g_capturing) {
        const int left = (int)((g_cap_until - std::min(g_cap_until, GetTickCount64())) / 1000) + 1;
        text_dim("Press the new key now (optionally with Ctrl / Shift / Alt). Mouse buttons are ignored, Esc cancels. "
                 + std::to_string(left) + " s", ui::kAmber);
        if (ui::danger("Cancel##capx", S(160), bh)) capture_cancel("Cancelled - hotkey unchanged");
    } else {
        if (ui::primary("Change...##hk", S(160), bh)) capture_begin();
        ImGui::SameLine();
        if (ui::primary("Reset to INSERT##hkr", S(180), bh)) { g_settings.hotkey = Hotkey(); settings_commit("Panel hotkey reset to INSERT"); }
    }
    kv_row("Controller", "LB + D-pad Down opens / closes the panel, Start closes it");

    section_header("Display");
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted("Panel scale");
    ImGui::SameLine(S(190));
    ImGui::SetNextItemWidth(std::min(S(320.f), ImGui::GetContentRegionAvail().x));
    float v = g_scale_edit > 0 ? g_scale_edit : g_settings.ui_scale;
    if (ImGui::SliderFloat("##uiscale", &v, 0.75f, 1.5f, "%.2fx")) g_scale_edit = std::round(v * 20.f) / 20.f;
    if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) { g_scale_edit = -1.f; g_settings.ui_scale = 1.f; settings_commit("Panel scale reset to 1.00x"); }
    else if (ImGui::IsItemDeactivatedAfterEdit() && g_scale_edit > 0) {
        g_settings.ui_scale = std::clamp(g_scale_edit, 0.75f, 1.5f); g_scale_edit = -1.f;
        char b[64]; sprintf_s(b, "Panel scale %.2fx", g_settings.ui_scale); settings_commit(b);
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Applies when you let go. Right-click: back to 1.00x");
    {   ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted("Classic look");
        ImGui::SameLine(S(190));
        bool cl = sk::g_classic;
        if (toggle_switch("##classic", &cl)) { sk::g_classic = cl; sk::g_loaded = false; }
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Off: the Borderlands skin (bin\\skin). On: the plain panel. Toggling also reloads skin.json."); }


    section_header("Crash logs");
    text_dim("If the game crashes, the trainer keeps that session's log in the logs\\crashes folder, so it is not overwritten. "
             "Send the newest crashes_###.log file (or a trainer report, below) when you report a crash.", ui::kDim);
    {   const int n = crash_log_count();
        text_dim(std::to_string(n) + " crash log" + (n == 1 ? "" : "s") + " saved (20 per file, old ones are always kept)." +
                 (crash_last_session() ? " The last game session ended unexpectedly." : ""), crash_last_session() ? ui::kAmber : ui::kDim);
        ImGui::Dummy(ImVec2(0, S(2)));
        if (ui::primary("Open crash logs folder##crf", S(230), bh) && !crash_log_dir().empty()) {
            CreateDirectoryA(crash_log_dir().c_str(), nullptr);
            ShellExecuteA(nullptr, "open", crash_log_dir().c_str(), nullptr, nullptr, SW_SHOWNORMAL); }
        ImGui::SameLine();
        if (ui::primary("Open logs folder##lgf", S(190), bh)) ShellExecuteA(nullptr, "open", (g_cfg.root + "\\logs").c_str(), nullptr, nullptr, SW_SHOWNORMAL);
    }

    // trainer report (report.cpp): one button -> logs\reports\trainer_report_<time>.txt + clipboard, for support
    section_header("Support");
    {   // made on a worker thread: it asks the game thread for the trainer state, the render thread must never wait on it
        static std::mutex rep_mtx; static std::string rep_path, rep_verdict, rep_text; static std::atomic<int> rep_busy{0};   // 0 idle, 1 making, 2 done
        if (rep_busy == 2) {
            std::string text; { std::lock_guard<std::mutex> lk(rep_mtx); text = rep_text; rep_text.clear(); }
            rep_busy = 0;
            const bool clip = !text.empty() && clipboard_set(text);
            settings_commit(text.empty() ? "Could not make the trainer report" : clip ? "Trainer report made and copied to the clipboard" : "Trainer report made (copy it from the file)");
        }
        text_dim("Something not working? Make a trainer report and post it where you get support. It checks the trainer, "
                 "lists any errors and crashes, and says whether the problem is the trainer or something else. Your user name "
                 "and folders are removed. Load your character first for a complete report.", ui::kDim);
        ImGui::Dummy(ImVec2(0, S(2)));
        if (rep_busy == 1) ImGui::TextDisabled("Making the trainer report...");
        else if (ui::primary("Create trainer report##rep", S(230), bh)) {
            rep_busy = 1;
            std::thread([] {
                std::string path, text;
                try { text = report_create(&path); } catch (std::exception& e) { log("[report] failed: %s", e.what()); } catch (...) { log("[report] failed"); }
                std::lock_guard<std::mutex> lk(rep_mtx);
                if (!text.empty()) { const size_t a = text.find('\n') + 1, b = text.find('\n', a); rep_verdict = text.substr(a, b - a); rep_path = path; }
                rep_text = text; rep_busy = 2;
            }).detach();
        }
        std::lock_guard<std::mutex> lk(rep_mtx);
        if (!rep_path.empty()) {
            ImGui::SameLine();
            if (ui::primary("Show the file##repshow", S(170), bh)) show_in_explorer(rep_path);
            const bool bad = rep_verdict.rfind("TRAINER PROBLEM", 0) == 0, chk = rep_verdict.rfind("TRAINER CHECK", 0) == 0;
            ImGui::Dummy(ImVec2(0, S(2)));
            text_dim(rep_verdict, bad ? ui::kRed : chk ? ui::kAmber : ui::kGreen);
            text_dim("Saved as " + rep_path.substr(rep_path.find_last_of("\\/") + 1) + " and copied to the clipboard.", ui::kDim);
        }
    }

    section_header("About");
    kv_row("Version", kVersion);
    kv_row("Settings file", g_cfg.root + "\\settings.json");
    if (!g_set_msg.empty() && GetTickCount64() - g_set_at < 8000) { ImGui::Dummy(ImVec2(0, S(4))); text_dim(g_set_msg, g_set_ok ? ui::kGreen : ui::kRed); }
}

struct PanelScale {
    float base, fgs;
    explicit PanelScale(float k) {
        ImGuiIO& io = ImGui::GetIO();
        base = g_ui_scale; fgs = io.FontGlobalScale;
        g_ui_scale = base * k; io.FontGlobalScale = fgs * k;
    }
    ~PanelScale() {
        ImGui::GetIO().FontGlobalScale = fgs; g_ui_scale = base;
        ImGui::PushFont(ImGui::GetFont()); ImGui::PopFont();   // recompute the current font size for whatever draws next (toasts)
    }
};

// ------------------------------------------------------------------ EDITOR page (fragments + gear)
namespace ed {

static const char* kRar[] = { "Common", "Uncommon", "Rare", "Legendary", "Mythical" };
static uint32_t rar_col(int r) {
    switch (r) { case 0: return 0xA8A294; case 1: return ui::kGreen; case 2: return ui::kBlue; case 3: return ui::kAmber; case 4: return 0xC77DDB; }
    return ui::kDim;
}
static const char* rar_name(int r) { return (r >= 0 && r <= 4) ? kRar[r] : "?"; }
static const char* type_name(int t) { return t == 0 ? "Modifier" : t == 1 ? "Player skill" : t == 2 ? "Dragon skill" : "?"; }

static std::vector<json> jlist(const json& j) {
    std::vector<json> out;
    if (j.is_array()) { for (auto& x : j) out.push_back(x); return out; }
    if (j.is_object()) {
        std::vector<std::pair<long long, json>> v;
        for (auto& kv : j.items()) {
            char* e = nullptr; const long long k = strtoll(kv.key().c_str(), &e, 10);
            if (kv.key().empty() || !e || *e) return {};          // a record, not a list
            v.emplace_back(k, kv.value());
        }
        std::sort(v.begin(), v.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
        for (auto& x : v) out.push_back(std::move(x.second));
    }
    return out;
}
static std::vector<json> jlist_at(const json& j, const char* k) { auto it = j.find(k); return it == j.end() ? std::vector<json>() : jlist(*it); }
static int jrar(const json& j, const char* k) {
    auto it = j.find(k); if (it == j.end()) return -1;
    if (it->is_number()) return (int)it->get<double>();
    if (it->is_string()) {
        std::string s = it->get<std::string>();
        if (s.find("Uncommon") != std::string::npos) return 1;
        for (int i = 4; i >= 0; --i) if (s.find(kRar[i]) != std::string::npos) return i;
    }
    return -1;
}
static std::string fnum(double d) {
    char b[64];
    if (std::fabs(d - std::round(d)) < 1e-6 && std::fabs(d) < 1e15) sprintf_s(b, "%.0f", d);
    else { sprintf_s(b, "%.3f", d); size_t n = strlen(b); while (n && b[n - 1] == '0') b[--n] = 0; }
    return b;
}
static std::string pretty_tag(std::string t) {
    if (t.empty() || t == "None") return "(empty)";
    for (const char* p : { "GameplayCue.PlayerSkill.", "AbilityModifiers." })
        if (t.rfind(p, 0) == 0) { t = t.substr(strlen(p)); break; }
    if (t.size() > 6 && t.compare(t.size() - 6, 6, ".Skill") == 0) t.resize(t.size() - 6);
    return t;
}
static std::string gear_name(const std::string& tag) {   // last 2-3 tag parts
    std::vector<std::string> parts; size_t a = 0;
    while (a <= tag.size()) { size_t b = tag.find('.', a); if (b == std::string::npos) b = tag.size(); if (b > a) parts.push_back(tag.substr(a, b - a)); a = b + 1; }
    if (parts.empty()) return tag;
    const size_t take = parts.size() >= 4 ? 3 : std::min<size_t>(2, parts.size());
    std::string out;
    for (size_t i = parts.size() - take; i < parts.size(); ++i) out += (out.empty() ? "" : " ") + parts[i];
    return out;
}
static std::string lower(std::string s) { std::transform(s.begin(), s.end(), s.begin(), ::tolower); return s; }

struct Slot { std::string tag, name, desc; int rarity = 0, type = 0; bool divine = false; };
static std::string slot_label(const std::string& tag, const std::string& name) { return name.empty() ? pretty_tag(tag) : name; }
// "HealthMaxMultiplier" -> "Health Max Multiplier" (gear attributes: the game's own names need an FGameplayAttribute
static std::string pretty_attr(const std::string& a) {
    std::string out;
    for (size_t i = 0; i < a.size(); ++i) {
        const char c = a[i];
        if (i && isupper((unsigned char)c) && (islower((unsigned char)a[i - 1]) || (i + 1 < a.size() && islower((unsigned char)a[i + 1]))))
            out += ' ';
        out += c;
    }
    return out;
}
struct Frag { int64_t handle = 0; int rarity = 0; bool onGrid = false, floating = false; int size = 0; std::vector<Slot> slots; };
struct Affix { std::string attr; double value = 0, eff = 0; bool divine = false, primary = false, corrupted = false; };
struct Gear { int64_t handle = 0; bool equipped = false; std::string tag, name; int level = 0, rarity = -1, rerolls = -1; std::vector<Affix> affixes;
              bool rav = false; std::string rav_name, rav_text; int rav_left = -1, rav_max = 0; };

struct SlotUI {          // key "handle:slot"
    std::vector<std::string> opts, opt_names, opt_desc; bool loaded = false; int64_t opts_seq = 0;
    int sel = -1;        // index into opts, -1 = keep the current tag
    int rarity = -1;     // -1 = not initialised from the slot yet
    char filter[64] = {};
    int64_t apply_seq = 0;
};
struct AttrOpt { std::string attr; double mn = 0, mx = 0; bool has_range = false; };
struct AffixUI {         // key "handle:affix"
    float quality = 5.f; std::vector<Affix> opts; bool rolled = false; int64_t opts_seq = 0;
    int sel = -1; double value = 0; bool divine = false; int64_t apply_seq = 0;
    std::vector<AttrOpt> attrs; bool attrs_loaded = false; int64_t attrs_seq = 0; int attr_sel = -1; int64_t pick_seq = 0;
};
struct Msg { std::string text; bool ok = true; };

static int g_sub = 0;                       // 0 FRAGMENTS, 1 GEAR
static std::vector<Frag> g_frags; static bool g_frags_loaded = false; static int64_t g_frags_seq = 0;
static char g_ffilter[96] = {}; static int64_t g_fsel = 0; static std::map<int64_t, int64_t> g_fmax_seq;
static std::vector<Gear> g_gear; static bool g_gear_loaded = false; static int64_t g_gear_seq = 0;
static char g_gfilter[96] = {}; static int64_t g_gsel = 0;
static std::map<std::string, SlotUI> g_slot_ui;
static std::map<std::string, AffixUI> g_aff_ui;
static std::map<std::string, Msg> g_msgs;

static std::string key(int64_t h, int i) { return std::to_string(h) + ":" + std::to_string(i); }
static void set_msg(const std::string& k, const std::string& t, bool okv) { g_msgs[k] = Msg{ t, okv }; }
static void show_msg(const std::string& k) {
    auto m = g_msgs.find(k); if (m == g_msgs.end() || m->second.text.empty()) return;
    text_dim((m->second.ok ? "> " : "! ") + m->second.text, m->second.ok ? ui::kGreen : ui::kRed);
}
static void loading_line(const char* what) { text_dim(std::string(what) + " loading...", ui::kAmber); }

static Frag parse_frag(const json& j) {
    Frag f; f.handle = (int64_t)jnum(j, "handle", 0); f.rarity = jrar(j, "rarity");
    f.onGrid = jbool(j, "onGrid", false); f.floating = jbool(j, "floating", false);
    for (auto& s : jlist_at(j, "slots")) if (s.is_object()) {
        Slot x; x.tag = jstr(s, "tag"); x.name = jstr(s, "name"); x.desc = jstr(s, "desc"); x.rarity = jrar(s, "rarity"); x.type = (int)jnum(s, "type", -1); x.divine = jbool(s, "divine", false);
        f.slots.push_back(x);
    }
    f.size = (int)jnum(j, "size", (double)f.slots.size());
    return f;
}
static Affix parse_affix(const json& j) {
    Affix a; a.attr = jstr(j, "attr", "?"); a.value = jnum(j, "value", 0); a.eff = jnum(j, "eff", 0);
    a.divine = jbool(j, "divine", false); a.primary = jbool(j, "primary", false); a.corrupted = jbool(j, "corrupted", false);
    return a;
}
static Gear parse_gear(const json& j) {
    Gear g; g.handle = (int64_t)jnum(j, "handle", 0); g.equipped = jbool(j, "equipped", false); g.tag = jstr(j, "tag"); g.name = jstr(j, "name");
    g.level = (int)jnum(j, "level", 0); g.rarity = jrar(j, "rarity"); g.rerolls = (int)jnum(j, "rerollsLeft", -1);
    for (auto& a : jlist_at(j, "affixes")) if (a.is_object()) g.affixes.push_back(parse_affix(a));
    auto rv = j.find("ravenous");
    if (rv != j.end() && rv->is_object()) { g.rav = true; g.rav_name = jstr(*rv, "name"); g.rav_text = jstr(*rv, "text");
        g.rav_left = (int)jnum(*rv, "left", -1); g.rav_max = (int)jnum(*rv, "max", 0); }
    return g;
}
static Frag* find_frag(int64_t h) { for (auto& f : g_frags) if (f.handle == h) return &f; return nullptr; }
static Gear* find_gear(int64_t h) { for (auto& g : g_gear) if (g.handle == h) return &g; return nullptr; }
static std::string err_text(const json& res) { std::string s = as_text(res); return s.empty() ? "failed" : s; }

// ---- requests
static void refresh_frags() {
    g_frags_seq = lua_call("DKE.fragments", json::array(), [](bool okv, const json& res) {
        g_frags_seq = 0;
        if (!okv) { set_msg("frags", "Refresh failed: " + err_text(res), false); return; }
        std::vector<Frag> v;
        for (auto& x : jlist(res)) if (x.is_object()) v.push_back(parse_frag(x));
        g_frags = std::move(v); g_frags_loaded = true;
        set_msg("frags", std::to_string(g_frags.size()) + " fragments", true);
    });
}
static void refresh_gear() {
    g_gear_seq = lua_call("DKE.gear", json::array(), [](bool okv, const json& res) {
        g_gear_seq = 0;
        if (!okv) { set_msg("gear", "Refresh failed: " + err_text(res), false); return; }
        std::vector<Gear> v;
        for (auto& x : jlist(res)) if (x.is_object()) v.push_back(parse_gear(x));
        std::stable_sort(v.begin(), v.end(), [](const Gear& a, const Gear& b) { return a.equipped && !b.equipped; });
        g_gear = std::move(v); g_gear_loaded = true;
        set_msg("gear", std::to_string(g_gear.size()) + " items", true);
    });
}
static void fetch_frag_options(int64_t h, int slot) {
    const std::string k = key(h, slot);
    g_slot_ui[k].opts_seq = lua_call("DKE.frag_options", json::array({ h, slot }), [k, h, slot](bool okv, const json& res) {
        SlotUI& u = g_slot_ui[k]; u.opts_seq = 0; u.loaded = true;
        if (!okv) { set_msg("fs:" + k, "Options failed: " + err_text(res), false); return; }
        u.opts.clear(); u.opt_names.clear(); u.opt_desc.clear();
        for (auto& x : jlist(res)) {
            if (x.is_string()) { u.opts.push_back(x.get<std::string>()); u.opt_names.emplace_back(); u.opt_desc.emplace_back(); }
            else if (x.is_object() && !jstr(x, "tag").empty()) {
                u.opts.push_back(jstr(x, "tag")); u.opt_names.push_back(jstr(x, "name")); u.opt_desc.push_back(jstr(x, "desc"));
            }
        }
        u.sel = -1;
        if (Frag* f = find_frag(h)) if (slot >= 1 && slot <= (int)f->slots.size())
            for (int i = 0; i < (int)u.opts.size(); ++i) if (u.opts[i] == f->slots[slot - 1].tag) u.sel = i;
    });
}
static void apply_frag_slot(int64_t h, int slot, const std::string& tag, int rarity, bool divine) {
    const std::string k = key(h, slot);
    set_msg("fs:" + k, "", true);
    g_slot_ui[k].apply_seq = lua_call("DKE.frag_set", json::array({ h, slot, tag, rarity, divine }), [k, h, slot, tag](bool okv, const json& res) {
        SlotUI& u = g_slot_ui[k]; u.apply_seq = 0;
        if (!okv) { set_msg("fs:" + k, "Apply failed: " + err_text(res), false); return; }
        const bool inner = res.is_object() ? jbool(res, "ok", false) : false;
        auto fr = res.is_object() ? res.find("fragment") : res.end();
        if (res.is_object() && fr != res.end() && fr->is_object()) {
            Frag nf = parse_frag(*fr);
            if (Frag* f = find_frag(h)) *f = nf; else g_frags.push_back(nf);
        }
        u.loaded = false; u.opts.clear(); u.sel = -1; u.rarity = -1;   // re-read from the updated slot
        std::string now_label = pretty_tag(tag);
        if (Frag* f = find_frag(h)) if (slot >= 1 && slot <= (int)f->slots.size()) now_label = slot_label(f->slots[slot - 1].tag, f->slots[slot - 1].name);
        if (!inner) set_msg("fs:" + k, "Game call returned false - slot unchanged? (" + as_text(res) + ")", false);
        else set_msg("fs:" + k, "Applied: " + now_label, true);
    });
}
static void frag_max(int64_t h) {
    set_msg("fmax:" + std::to_string(h), "", true);
    g_fmax_seq[h] = lua_call("DKE.frag_max", json::array({ h }), [h](bool okv, const json& res) {
        g_fmax_seq[h] = 0;
        const std::string mk = "fmax:" + std::to_string(h);
        if (!okv) { set_msg(mk, "Failed: " + err_text(res), false); return; }
        const json* fj = nullptr;
        if (res.is_object() && res.contains("slots")) fj = &res;
        else if (res.is_object() && res.contains("fragment") && res["fragment"].is_object()) fj = &res["fragment"];
        if (fj) { Frag nf = parse_frag(*fj); if (Frag* f = find_frag(h)) *f = nf; else g_frags.push_back(nf); }
        for (auto& kv : g_slot_ui) if (kv.first.rfind(std::to_string(h) + ":", 0) == 0) kv.second.rarity = -1;
        set_msg(mk, fj ? "Every slot is now Mythical" : "Done (no fragment data returned - refresh)", true);
    });
}
static void roll_gear_options(int64_t h, int ai, float quality) {
    const std::string k = key(h, ai);
    set_msg("ga:" + k, "", true);
    const double q = std::round(quality * 10.0) / 10.0;
    g_aff_ui[k].opts_seq = lua_call("DKE.gear_options", json::array({ h, ai, q }), [k](bool okv, const json& res) {
        AffixUI& u = g_aff_ui[k]; u.opts_seq = 0;
        if (!okv) { set_msg("ga:" + k, "Roll failed: " + err_text(res), false); return; }
        u.opts.clear(); u.sel = -1; u.rolled = true;
        for (auto& x : jlist(res)) if (x.is_object()) u.opts.push_back(parse_affix(x));
        set_msg("ga:" + k, u.opts.empty() ? "The game offered no options for this affix" : std::to_string(u.opts.size()) + " options - pick one", !u.opts.empty());
    });
}
// every attribute the game allows on this affix slot (DKE.gear_attrs), with the value range for this item
static void fetch_gear_attrs(int64_t h, int ai) {
    const std::string k = key(h, ai);
    g_aff_ui[k].attrs_seq = lua_call("DKE.gear_attrs", json::array({ h, ai }), [k](bool okv, const json& res) {
        AffixUI& u = g_aff_ui[k]; u.attrs_seq = 0; u.attrs_loaded = true;
        if (!okv) { set_msg("ga:" + k, "Attribute list failed: " + err_text(res), false); return; }
        u.attrs.clear(); u.attr_sel = -1;
        for (auto& x : jlist(res)) if (x.is_object()) {
            AttrOpt o; o.attr = jstr(x, "attr", "?");
            o.has_range = x.contains("min") && x.contains("max") && x["min"].is_number() && x["max"].is_number();
            if (o.has_range) { o.mn = x["min"].get<double>(); o.mx = x["max"].get<double>(); }
            u.attrs.push_back(o);
        }
    });
}
// roll the game's generator until it offers the chosen attribute (DKE.gear_pick); the hit becomes option 1
static void pick_gear_attr(int64_t h, int ai, const std::string& attr, float quality) {
    const std::string k = key(h, ai);
    set_msg("ga:" + k, "", true);
    const double q = std::round(quality * 10.0) / 10.0;
    g_aff_ui[k].pick_seq = lua_call("DKE.gear_pick", json::array({ h, ai, attr, q, 150 }), [k](bool okv, const json& res) {
        AffixUI& u = g_aff_ui[k]; u.pick_seq = 0;
        if (!okv || !res.is_object()) { set_msg("ga:" + k, "Not offered: " + err_text(res), false); return; }
        Affix a = parse_affix(res);
        u.opts.clear(); u.opts.push_back(a); u.rolled = true;
        u.sel = 0; u.value = a.value; u.divine = a.divine;
        set_msg("ga:" + k, "Got " + pretty_attr(a.attr) + " " + fnum(a.value) + " after " + fnum(jnum(res, "rolls", 0)) + " roll(s) - set the value, then APPLY", true);
    });
}
static void apply_gear_affix(int64_t h, int ai, int opt1, double value, bool divine) {
    const std::string k = key(h, ai);
    set_msg("ga:" + k, "", true);
    g_aff_ui[k].apply_seq = lua_call("DKE.gear_set", json::array({ h, ai, opt1, value, divine }), [k, h, ai](bool okv, const json& res) {
        AffixUI& u = g_aff_ui[k]; u.apply_seq = 0;
        if (!okv) { set_msg("ga:" + k, "Apply failed: " + err_text(res), false); return; }
        const bool inner = res.is_object() ? jbool(res, "ok", false) : false;
        auto it = res.is_object() ? res.find("item") : res.end();
        if (res.is_object() && it != res.end() && it->is_object()) {
            Gear ng = parse_gear(*it);
            if (Gear* g = find_gear(h)) { ng.equipped = g->equipped; *g = ng; }   // gear_set always reports equipped=true
            else g_gear.push_back(ng);
        }
        u.opts.clear(); u.sel = -1; u.rolled = false;       // roll again for the next change
        u.attrs_loaded = false;                             // ranges / allowed list can change with the item
        std::string now;
        if (Gear* g = find_gear(h)) if (ai >= 1 && ai <= (int)g->affixes.size()) now = pretty_attr(g->affixes[ai - 1].attr) + " " + fnum(g->affixes[ai - 1].value);
        if (!inner) set_msg("ga:" + k, "Game call returned false - affix unchanged? (" + as_text(res) + ")", false);
        else set_msg("ga:" + k, "Applied: " + now, true);
    });
}

// ---- widgets
static bool list_row(const char* id, const std::string& left, const std::string& right, uint32_t col, bool sel) {
    ImDrawList* dl = ImGui::GetWindowDrawList();
    const float h = ImGui::GetFrameHeight() + S(4), w = ImGui::GetContentRegionAvail().x;
    const ImVec2 p = ImGui::GetCursorScreenPos();
    const bool clicked = ImGui::InvisibleButton(id, ImVec2(w, h));
    const bool hov = ImGui::IsItemHovered();
    if (sel) { dl->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h), ui::rgb(ui::kGold, .20f), ui::rgb(ui::kGold, .02f), ui::rgb(ui::kGold, .02f), ui::rgb(ui::kGold, .20f));
               ui::gold_frame(dl, p, ImVec2(p.x + w - 1, p.y + h - 1), 1.f, .5f); }
    else if (hov) dl->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::rgb(0xFFFFFF, .04f));
    dl->AddRectFilled(p, ImVec2(p.x + S(3), p.y + h), ui::rgb(col));
    const ImVec2 ts = ImGui::CalcTextSize(left.c_str()), rs = ImGui::CalcTextSize(right.c_str());
    dl->PushClipRect(p, ImVec2(p.x + w - rs.x - S(14), p.y + h), true);
    dl->AddText(ImVec2(p.x + S(10), p.y + (h - ts.y) * .5f), ui::rgb(col, sel || hov ? 1.f : .9f), left.c_str());
    dl->PopClipRect();
    dl->AddText(ImVec2(p.x + w - rs.x - S(8), p.y + (h - rs.y) * .5f), ui::rgb(sel ? ui::kGoldHi : ui::kDim), right.c_str());
    return clicked;
}
static void colored(const std::string& s, uint32_t c) { ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(c)); ImGui::TextUnformatted(s.c_str()); ImGui::PopStyleColor(); }

static void toolbar(const char* id, bool online, bool loading, char* filter, size_t fsz, const char* hint, void (*refresh)()) {
    ImGui::PushID(id);
    ImGui::BeginDisabled(!online || loading);
    if (ui::primary(loading ? "LOADING...###r" : "REFRESH###r", S(130), ImGui::GetFrameHeight())) refresh();
    ImGui::EndDisabled();
    if (!online && ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip(LINK_OFF);
    ImGui::SameLine(0, S(8));
    ImGui::SetNextItemWidth(std::min(S(320), ImGui::GetContentRegionAvail().x));
    ImGui::InputTextWithHint("##f", hint, filter, fsz);
    ImGui::PopID();
}

// ---- FRAGMENTS
static bool frag_matches(const Frag& f, const std::string& flt) {
    if (flt.empty()) return true;
    for (auto& s : f.slots) if (lower(s.tag).find(flt) != std::string::npos || lower(s.name).find(flt) != std::string::npos) return true;
    return false;
}
static void draw_frag_slot(Frag& f, int si, bool online) {
    Slot& s = f.slots[si - 1];
    const std::string k = key(f.handle, si);
    SlotUI& u = g_slot_ui[k];
    if (u.rarity < 0) u.rarity = std::clamp(s.rarity, 0, 4);
    ImGui::PushID(si);
    char head[300];
    _snprintf_s(head, sizeof head, _TRUNCATE, "Slot %d   %s###slot", si, slot_label(s.tag, s.name).c_str());
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(rar_col(s.rarity)));
    const bool open = ImGui::CollapsingHeader(head);
    ImGui::PopStyleColor();
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s%s%s", s.tag.c_str(), s.desc.empty() ? "" : "\n", s.desc.c_str());
    {   // right-hand info: fixed column, pushed right when the name is long (never over the name)
        const char* vis_end = strstr(head, "###");
        const float name_w = ImGui::CalcTextSize(head, vis_end).x;
        const float name_end = ImGui::GetItemRectMin().x - ImGui::GetWindowPos().x + ImGui::GetFontSize()
                             + ImGui::GetStyle().FramePadding.x * 3 + name_w + S(14);
        ImGui::SameLine(std::max(ImGui::GetWindowContentRegionMax().x - S(230), name_end));
    }
    ImGui::TextDisabled("%s  %s%s", rar_name(s.rarity), type_name(s.type), s.divine ? "  Divine" : "");
    if (open) {
        ImGui::Indent(S(12));
        if (!s.desc.empty()) ImGui::TextWrapped("%s", s.desc.c_str());
        text_dim("Tag: " + (s.tag.empty() ? std::string("(empty)") : s.tag));
        if (!u.loaded && !call_busy(u.opts_seq) && online) fetch_frag_options(f.handle, si);   // fetched when expanded
        const bool busy_opts = call_busy(u.opts_seq), busy_apply = call_busy(u.apply_seq);
        const float lw = S(70);
        // tag combo (with a filter field inside)
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Tag"); ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(std::min(S(440), ImGui::GetContentRegionAvail().x - S(80)));
        auto opt_label = [&u](int i) { return slot_label(u.opts[i], i < (int)u.opt_names.size() ? u.opt_names[i] : std::string()); };
        const std::string preview = u.sel >= 0 && u.sel < (int)u.opts.size() ? opt_label(u.sel) : busy_opts ? std::string("loading...") : "(keep current) " + slot_label(s.tag, s.name);
        ImGui::BeginDisabled(busy_opts || u.opts.empty());
        if (ImGui::BeginCombo("##tag", preview.c_str(), ImGuiComboFlags_HeightLarge)) {
            if (ImGui::IsWindowAppearing()) ImGui::SetKeyboardFocusHere();
            ImGui::SetNextItemWidth(-1);
            ImGui::InputTextWithHint("##flt", "Filter...", u.filter, sizeof u.filter);
            const std::string fl = lower(u.filter);
            if (ImGui::Selectable("(keep current)", u.sel < 0)) u.sel = -1;
            for (int i = 0; i < (int)u.opts.size(); ++i) {
                const std::string lab = opt_label(i);
                if (!fl.empty() && lower(u.opts[i]).find(fl) == std::string::npos && lower(lab).find(fl) == std::string::npos) continue;
                ImGui::PushID(i);
                const bool cur = u.opts[i] == s.tag;
                if (ImGui::Selectable((lab + (cur ? "   (current)" : "")).c_str(), u.sel == i)) u.sel = i;
                if (ImGui::IsItemHovered()) {
                    const std::string d = i < (int)u.opt_desc.size() ? u.opt_desc[i] : std::string();
                    ImGui::SetTooltip("%s%s%s", u.opts[i].c_str(), d.empty() ? "" : "\n", d.c_str());
                }
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        ImGui::SameLine();
        ImGui::BeginDisabled(!online || busy_opts);
        if (ImGui::SmallButton("refresh")) { u.loaded = false; fetch_frag_options(f.handle, si); }
        ImGui::EndDisabled();
        if (busy_opts) loading_line("Options");
        else if (u.loaded && u.opts.empty()) text_dim("No options from the game for this slot", ui::kAmber);
        // rarity
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Rarity"); ImGui::SameLine(lw);
        ImGui::SetNextItemWidth(S(180));
        ImGui::Combo("##rar", &u.rarity, kRar, 5);
        // divine: does not stick on fragments
        ImGui::SameLine(0, S(16));
        ImGui::BeginDisabled(true); bool dv = s.divine; ImGui::Checkbox("Divine", &dv); ImGui::EndDisabled();
        ImGui::SameLine(); ImGui::TextDisabled("(does not stick on fragments)");
        // apply
        ImGui::Dummy(ImVec2(0, S(2)));
        ImGui::BeginDisabled(!online || busy_apply || busy_opts);
        if (ui::primary(busy_apply ? "APPLYING...###ap" : "APPLY###ap", S(140), ImGui::GetFrameHeight() + S(2))) {
            const std::string tag = u.sel >= 0 && u.sel < (int)u.opts.size() ? u.opts[u.sel] : s.tag;   // never null
            apply_frag_slot(f.handle, si, tag, u.rarity, s.divine);
        }
        ImGui::EndDisabled();
        if (busy_apply) loading_line("Apply");
        show_msg("fs:" + k);
        ImGui::Unindent(S(12));
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    ImGui::PopID();
}
static void draw_fragments(bool online) {
    toolbar("frag", online, call_busy(g_frags_seq), g_ffilter, sizeof g_ffilter, "Filter by any slot tag...", refresh_frags);
    show_msg("frags");
    if (!g_frags_loaded) { if (!call_busy(g_frags_seq)) text_dim(online ? "Press REFRESH to read your fragments from the game." : LINK_OFF ".", ui::kDim); else loading_line("Fragments"); return; }
    const std::string flt = lower(g_ffilter);
    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##flist", ImVec2(S(330), avail_h), ImGuiChildFlags_Borders);
    int shown = 0;
    for (auto& f : g_frags) {
        if (!frag_matches(f, flt)) continue;
        ++shown;
        ImGui::PushID((int)(f.handle & 0x7FFFFFFF));
        const std::string name = f.slots.empty() ? "(no slots)" : slot_label(f.slots[0].tag, f.slots[0].name);
        const std::string right = std::to_string(f.size) + (f.onGrid ? "  GRID" : f.floating ? "  FLOAT" : "  STASH");
        if (list_row("##r", name, right, rar_col(f.rarity), g_fsel == f.handle)) g_fsel = f.handle;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s  -  %s, %d slots, handle %lld", f.onGrid ? "on the grid" : f.floating ? "floating" : "in the stash", rar_name(f.rarity), f.size, (long long)f.handle);
        ImGui::PopID();
    }
    if (!shown) ImGui::TextDisabled(g_frags.empty() ? "No fragments." : "Nothing matches the filter.");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##fdet", ImVec2(0, avail_h), ImGuiChildFlags_Borders);
    Frag* f = find_frag(g_fsel);
    if (!f) text_dim("Select a fragment on the left.");
    else {
        const Frag& cf = *f;
        ImGui::PushFont(g_font_bold);
        colored(cf.slots.empty() ? "(no slots)" : slot_label(cf.slots[0].tag, cf.slots[0].name), rar_col(cf.rarity));
        ImGui::PopFont();
        ImGui::TextDisabled("%s  |  %d slots  |  %s  |  handle %lld", rar_name(cf.rarity), cf.size,
                            cf.onGrid ? "on the grid" : cf.floating ? "floating" : "in the stash", (long long)cf.handle);
        const int64_t h = cf.handle;
        const bool busy_max = call_busy(g_fmax_seq[h]);
        ImGui::BeginDisabled(!online || busy_max);
        if (ui::primary(busy_max ? "WORKING...###max" : "MAKE ALL SLOTS MYTHICAL###max", S(260), ImGui::GetFrameHeight() + S(2))) frag_max(h);
        ImGui::EndDisabled();
        if (busy_max) loading_line("Make Mythical");
        show_msg("fmax:" + std::to_string(h));
        section_header("Slots");
        f = find_frag(h);   // a callback may have replaced it (same lock, but keep it obviously safe)
        if (f) for (int si = 1; si <= (int)f->slots.size(); ++si) draw_frag_slot(*f, si, online);
    }
    ImGui::EndChild();
}

// ---- GEAR
static bool gear_matches(const Gear& g, const std::string& flt) {
    if (flt.empty()) return true;
    if (lower(g.tag).find(flt) != std::string::npos || lower(g.name).find(flt) != std::string::npos) return true;
    for (auto& a : g.affixes) if (lower(a.attr).find(flt) != std::string::npos) return true;
    return false;
}
static void draw_affix(Gear& g, int ai, bool online) {
    Affix& a = g.affixes[ai - 1];
    const std::string k = key(g.handle, ai);
    AffixUI& u = g_aff_ui[k];
    ImGui::PushID(ai);
    char head[300];
    _snprintf_s(head, sizeof head, _TRUNCATE, "%d.  %s  %s%s%s%s###aff", ai, pretty_attr(a.attr).c_str(), fnum(a.value).c_str(),
              a.primary ? "   [primary]" : "", a.divine ? "   [divine]" : "", a.corrupted ? "   [corrupted]" : "");
    ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(a.divine ? ui::kGoldHi : a.corrupted ? ui::kRed : ui::kBone));
    const bool open = ImGui::CollapsingHeader(head);
    ImGui::PopStyleColor();
    if (open) {
        ImGui::Indent(S(12));
        if (a.eff != 0) text_dim("Effectiveness " + fnum(a.eff));
        const bool busy_roll = call_busy(u.opts_seq), busy_apply = call_busy(u.apply_seq);
        const bool busy_attrs = call_busy(u.attrs_seq), busy_pick = call_busy(u.pick_seq);
        const bool busy_any = busy_roll || busy_apply || busy_pick;
        if (!u.attrs_loaded && !busy_attrs && online) fetch_gear_attrs(g.handle, ai);   // fetched when expanded
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Quality"); ImGui::SameLine(S(80));
        ImGui::SetNextItemWidth(S(220));
        ImGui::SliderFloat("##q", &u.quality, 0.f, 10.f, "%.1f");
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right)) u.quality = 5.f;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Quality passed to the game's reroll generator (0..10). Right-click: 5");
        // pick any attribute the game allows on this slot
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Attribute"); ImGui::SameLine(S(80));
        ImGui::SetNextItemWidth(std::min(S(300), ImGui::GetContentRegionAvail().x - S(200)));
        auto attr_label = [](const AttrOpt& o) {
            return o.has_range ? pretty_attr(o.attr) + "   (" + fnum(o.mn) + " - " + fnum(o.mx) + ")" : pretty_attr(o.attr);
        };
        const std::string apreview = u.attr_sel >= 0 && u.attr_sel < (int)u.attrs.size() ? attr_label(u.attrs[u.attr_sel])
                                   : busy_attrs ? std::string("loading...") : u.attrs.empty() ? std::string("(none)") : std::string("choose...");
        ImGui::BeginDisabled(busy_attrs || u.attrs.empty());
        if (ImGui::BeginCombo("##attr", apreview.c_str(), ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < (int)u.attrs.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(attr_label(u.attrs[i]).c_str(), u.attr_sel == i)) u.attr_sel = i;
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Every attribute the game allows on this slot. The range is the normal roll range for this item level.");
        ImGui::SameLine(0, S(10));
        ImGui::BeginDisabled(!online || u.attr_sel < 0 || busy_any);
        if (ui::primary(busy_pick ? "ROLLING...###pick" : "GET THIS AFFIX###pick", S(170), ImGui::GetFrameHeight()))
            pick_gear_attr(g.handle, ai, u.attrs[u.attr_sel].attr, u.quality);
        ImGui::EndDisabled();
        if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
            ImGui::SetTooltip("Rolls the game's own generator until it offers this attribute (up to 150 rolls, free)");
        // or a plain random roll
        ImGui::Dummy(ImVec2(0, 0)); ImGui::SameLine(S(80));
        ImGui::BeginDisabled(!online || busy_any);
        if (ui::primary(busy_roll ? "ROLLING...###roll" : "RANDOM OPTIONS###roll", S(170), ImGui::GetFrameHeight())) roll_gear_options(g.handle, ai, u.quality);
        ImGui::EndDisabled();
        if (busy_roll || busy_pick) loading_line("Options");
        if (!u.opts.empty()) {
            ImGui::Dummy(ImVec2(0, S(2)));
            for (int i = 0; i < (int)u.opts.size(); ++i) {
                const Affix& o = u.opts[i];
                char lab[300];
                _snprintf_s(lab, sizeof lab, _TRUNCATE, "%s  %s%s%s##o%d", pretty_attr(o.attr).c_str(), fnum(o.value).c_str(), o.divine ? "   [divine]" : "", o.primary ? "   [primary]" : "", i);
                if (ImGui::RadioButton(lab, u.sel == i)) { u.sel = i; u.value = o.value; u.divine = o.divine; }
            }
            ImGui::Dummy(ImVec2(0, S(2)));
            ImGui::BeginDisabled(u.sel < 0);
            ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Value"); ImGui::SameLine(S(80));
            ImGui::SetNextItemWidth(S(160));
            ImGui::InputDouble("##val", &u.value, 0, 0, "%.3f");
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Prefilled with the option's value; any number works (e.g. 500)");
            ImGui::SameLine(0, S(16));
            ImGui::Checkbox("Divine", &u.divine);
            ImGui::SameLine(0, S(16));
            ImGui::EndDisabled();
            ImGui::BeginDisabled(!online || u.sel < 0 || busy_any);
            if (ui::primary(busy_apply ? "APPLYING...###ap" : "APPLY###ap", S(140), ImGui::GetFrameHeight()))
                apply_gear_affix(g.handle, ai, u.sel + 1, u.value, u.divine);
            ImGui::EndDisabled();
            if (u.sel < 0) text_dim("Pick one of the game's options above, then adjust value / divine.");
            if (busy_apply) loading_line("Apply");
        } else if (u.rolled && !busy_roll) text_dim("No options - try another quality.", ui::kAmber);
        show_msg("ga:" + k);
        ImGui::Unindent(S(12));
        ImGui::Dummy(ImVec2(0, S(4)));
    }
    ImGui::PopID();
}
// ---- Ravenous weapons (Editor > GEAR, 2026-11-09): make a weapon Ravenous, refill Voracity, devour with affix choice
struct RavUI {
    int64_t h = 0;                                   // item these choices belong to
    std::vector<std::pair<std::string, std::string>> choices; int64_t choices_seq = 0; bool choices_loaded = false; int csel = -1;
    int64_t make_seq = 0, refill_seq = 0, offer_seq = 0, eat_seq = 0; bool make_arm = false, eat_arm = false;
    int64_t food = 0; int slot = 1; std::vector<Affix> offer; int osel = -1; bool offered = false;
};
static RavUI g_rav;
static std::string rav_text(const std::string& t) {        // game rich text -> plain: <Tag>x</> -> x, {Value0} -> N
    std::string o;
    for (size_t i = 0; i < t.size(); ++i) {
        if (t[i] == '<') { size_t e = t.find('>', i); if (e != std::string::npos) { i = e; continue; } }
        if (t[i] == '{') { size_t e = t.find('}', i); if (e != std::string::npos) { o += t.compare(i, 13, "{ValuePercent") == 0 ? "N%" : "N"; i = e; continue; } }
        o += t[i];
    }
    return o;
}
static void rav_reset(int64_t h) { if (g_rav.h != h) { g_rav = RavUI(); g_rav.h = h; } }
static void rav_fetch_choices(int64_t h) {
    g_rav.choices_seq = lua_call("DKE.ravenous_choices", json::array({ h }), [h](bool okv, const json& res) {
        if (g_rav.h != h) return;
        g_rav.choices_seq = 0; g_rav.choices_loaded = true;
        if (!okv) { set_msg("rav", "Ravenous list failed: " + err_text(res), false); return; }
        for (auto& x : jlist(res)) if (x.is_object()) g_rav.choices.push_back({ jstr(x, "name"), rav_text(jstr(x, "text")) });
    });
}
static void rav_make(int64_t h, const std::string& name) {
    set_msg("rav", "", true);
    g_rav.make_seq = lua_call("DKE.ravenous_make", json::array({ h, name }), [name](bool okv, const json& res) {
        g_rav.make_seq = 0;
        if (!okv) { set_msg("rav", "Make failed: " + err_text(res), false); return; }
        set_msg("rav", "Now a Ravenous weapon: " + name + ". Re-open the inventory to see it.", true);
        refresh_gear();
    });
}
static void rav_refill(int64_t h) {
    g_rav.refill_seq = lua_call("DKE.voracity_refill", json::array({ h }), [h](bool okv, const json& res) {
        g_rav.refill_seq = 0;
        if (!okv) { set_msg("rav", "Refill failed: " + err_text(res), false); return; }
        if (Gear* g = find_gear(h)) { g->rav_left = (int)jnum(res, "left", g->rav_left); g->rav_max = (int)jnum(res, "max", g->rav_max); }
        set_msg("rav", "Voracity refilled", true);
    });
}
static void rav_offer(int64_t h) {
    g_rav.offer.clear(); g_rav.osel = -1; g_rav.offered = false; set_msg("rav", "", true);
    g_rav.offer_seq = lua_call("DKE.feed_options", json::array({ h, g_rav.food, g_rav.slot }), [h](bool okv, const json& res) {
        if (g_rav.h != h) return;
        g_rav.offer_seq = 0; g_rav.offered = true;
        if (!okv) { set_msg("rav", "Offer failed: " + err_text(res), false); return; }
        for (auto& a : jlist_at(res, "options")) if (a.is_object()) g_rav.offer.push_back(parse_affix(a));
        if (g_rav.offer.size() == 1) g_rav.osel = 0;
    });
}
static void rav_eat(int64_t h) {
    const int64_t food = g_rav.food; const int slot = g_rav.slot, opt = g_rav.osel + 1;
    g_rav.eat_seq = lua_call("DKE.feed_apply", json::array({ h, food, slot, opt, true }), [h, food](bool okv, const json& res) {
        g_rav.eat_seq = 0; g_rav.offer.clear(); g_rav.osel = -1; g_rav.offered = false; g_rav.food = 0;
        if (!okv) { set_msg("rav", "Devour failed: " + err_text(res), false); refresh_gear(); return; }
        auto it = res.is_object() ? res.find("item") : res.end();
        if (it != res.end() && it->is_object()) { Gear ng = parse_gear(*it); if (Gear* g = find_gear(h)) { ng.equipped = g->equipped; *g = ng; } }
        auto rv = res.is_object() ? res.find("ravenous") : res.end();
        if (Gear* g = find_gear(h)) if (rv != res.end() && rv->is_object()) {
            g->rav = true; g->rav_name = jstr(*rv, "name"); g->rav_text = jstr(*rv, "text"); g->rav_left = (int)jnum(*rv, "left", -1); g->rav_max = (int)jnum(*rv, "max", 0); }
        g_gear.erase(std::remove_if(g_gear.begin(), g_gear.end(), [food](const Gear& x) { return x.handle == food; }), g_gear.end());
        set_msg("rav", "Devoured - Voracity kept full. Re-open the inventory to see the new affix.", true);
    });
}
static bool is_weapon(const Gear& g) { const std::string t = g.tag.substr(g.tag.rfind('.') + 1); return t == "LeftHand" || t == "RightHand"; }
static std::string gear_label(const Gear& x) { return x.name.empty() ? gear_name(x.tag) : x.name; }
static std::string affix_label(const Gear& g, int i) { return std::to_string(i) + ".  " + pretty_attr(g.affixes[i - 1].attr) + " " + fnum(g.affixes[i - 1].value); }
static void draw_ravenous(Gear& g, bool online) {
    if (!is_weapon(g)) return;
    rav_reset(g.handle);
    section_header("Ravenous");
    const float bh = ImGui::GetFrameHeight();
    if (!g.rav) {
        if (g.equipped) { text_dim("Take this weapon off to turn it into a Ravenous weapon (bag items only)."); show_msg("rav"); return; }
        if (!g_rav.choices_loaded && !call_busy(g_rav.choices_seq) && online) rav_fetch_choices(g.handle);
        if (call_busy(g_rav.choices_seq)) { loading_line("Ravenous weapons"); return; }
        if (g_rav.choices.empty()) { text_dim("No Ravenous weapon for this class and hand."); show_msg("rav"); return; }
        ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Become"); ImGui::SameLine(S(80));
        ImGui::SetNextItemWidth(std::max(S(120), std::min(S(260), ImGui::GetContentRegionAvail().x - S(190))));   // room for the button
        const char* prev = g_rav.csel >= 0 && g_rav.csel < (int)g_rav.choices.size() ? g_rav.choices[g_rav.csel].first.c_str() : "choose...";
        if (ImGui::BeginCombo("##ravc", prev, ImGuiComboFlags_HeightLarge)) {
            for (int i = 0; i < (int)g_rav.choices.size(); ++i) {
                ImGui::PushID(i);
                if (ImGui::Selectable(g_rav.choices[i].first.c_str(), g_rav.csel == i)) { g_rav.csel = i; g_rav.make_arm = false; }
                if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", g_rav.choices[i].second.c_str());
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine(0, S(10));
        ImGui::BeginDisabled(!online || g_rav.csel < 0 || call_busy(g_rav.make_seq));
        if (ui::danger(g_rav.make_arm ? "CONFIRM?###ravm" : "MAKE RAVENOUS###ravm", S(170), bh)) {
            if (!g_rav.make_arm) g_rav.make_arm = true; else { g_rav.make_arm = false; rav_make(g.handle, g_rav.choices[g_rav.csel].first); }
        }
        ImGui::EndDisabled();
        if (g_rav.csel >= 0 && g_rav.csel < (int)g_rav.choices.size()) {
            ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - S(8)); text_dim(g_rav.choices[g_rav.csel].second, ui::kDim); ImGui::PopTextWrapPos(); }
        text_dim("Changes this item for good (keeps its level and affixes). Use a magic or rare weapon: it needs affixes to devour.", ui::kAmber);
        show_msg("rav");
        return;
    }
    // already Ravenous
    ImGui::PushFont(g_font_bold); colored(g.rav_name, ui::kRed); ImGui::PopFont();
    ImGui::SameLine(S(200)); ImGui::AlignTextToFramePadding();
    ImGui::TextDisabled("Voracity %s / %d", g.rav_left < 0 ? "?" : std::to_string(g.rav_left).c_str(), g.rav_max);
    ImGui::SameLine(0, S(12));
    ImGui::BeginDisabled(!online || call_busy(g_rav.refill_seq));
    if (ui::primary("REFILL VORACITY##ravr", S(170), bh)) rav_refill(g.handle);
    ImGui::EndDisabled();
    if (!g.rav_text.empty()) { ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - S(8)); text_dim(rav_text(g.rav_text), ui::kDim); ImGui::PopTextWrapPos(); }
    // devour
    ImGui::Dummy(ImVec2(0, S(4))); ui::label("Devour (free, Voracity stays full)", ui::kGold);
    if (g.affixes.empty()) { text_dim("This weapon has no affixes - devouring would fail. Give it affixes first.", ui::kAmber); show_msg("rav"); return; }
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Eat"); ImGui::SameLine(S(80));
    ImGui::SetNextItemWidth(std::min(S(320), ImGui::GetContentRegionAvail().x - S(10)));
    Gear* food = g_rav.food ? find_gear(g_rav.food) : nullptr;
    const std::string fprev = food ? gear_label(*food) + "  (Lv " + std::to_string(food->level) + ")" : std::string("choose an item from the bag...");
    if (ImGui::BeginCombo("##ravf", fprev.c_str(), ImGuiComboFlags_HeightLarge)) {
        for (auto& x : g_gear) {
            if (x.equipped || x.handle == g.handle || x.rav) continue;
            ImGui::PushID((int)(x.handle & 0x7FFFFFFF));
            const std::string lab = gear_label(x) + "  (" + rar_name(x.rarity) + ", Lv " + std::to_string(x.level) + ")";
            if (ImGui::Selectable(lab.c_str(), g_rav.food == x.handle)) { g_rav.food = x.handle; g_rav.offer.clear(); g_rav.osel = -1; g_rav.offered = false; g_rav.eat_arm = false; }
            if (ImGui::IsItemHovered()) { std::string tt = x.tag; for (auto& a : x.affixes) tt += "\n" + pretty_attr(a.attr) + " " + fnum(a.value); ImGui::SetTooltip("%s", tt.c_str()); }
            ImGui::PopID();
        }
        ImGui::EndCombo();
    }
    ImGui::AlignTextToFramePadding(); ImGui::TextUnformatted("Replace"); ImGui::SameLine(S(80));
    ImGui::SetNextItemWidth(std::min(S(320), ImGui::GetContentRegionAvail().x - S(10)));
    if (g_rav.slot < 1 || g_rav.slot > (int)g.affixes.size()) g_rav.slot = 1;
    if (ImGui::BeginCombo("##ravs", affix_label(g, g_rav.slot).c_str())) {
        for (int i = 1; i <= (int)g.affixes.size(); ++i)
            if (ImGui::Selectable(affix_label(g, i).c_str(), g_rav.slot == i)) { g_rav.slot = i; g_rav.offer.clear(); g_rav.osel = -1; g_rav.offered = false; g_rav.eat_arm = false; }
        ImGui::EndCombo();
    }
    if (ImGui::IsItemHovered()) ImGui::SetTooltip("Which of this weapon's affixes the devoured item replaces");
    ImGui::Dummy(ImVec2(0, 0)); ImGui::SameLine(S(80));
    ImGui::BeginDisabled(!online || !food || call_busy(g_rav.offer_seq) || call_busy(g_rav.eat_seq));
    if (ui::primary(call_busy(g_rav.offer_seq) ? "ASKING...###ravo" : "SEE NEW AFFIX###ravo", S(170), bh)) rav_offer(g.handle);
    ImGui::EndDisabled();
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled)) ImGui::SetTooltip("Asks the game which affix this item would give (nothing is eaten yet)");
    if (!g_rav.offer.empty()) {
        for (int i = 0; i < (int)g_rav.offer.size(); ++i) {
            const Affix& o = g_rav.offer[i]; char lab[300];
            _snprintf_s(lab, sizeof lab, _TRUNCATE, "%s  %s%s##ro%d", pretty_attr(o.attr).c_str(), fnum(o.value).c_str(), o.divine ? "   [divine]" : "", i);
            if (ImGui::RadioButton(lab, g_rav.osel == i)) { g_rav.osel = i; g_rav.eat_arm = false; }
        }
        ImGui::BeginDisabled(!online || g_rav.osel < 0 || call_busy(g_rav.eat_seq));
        if (ui::danger(call_busy(g_rav.eat_seq) ? "DEVOURING...###rave" : g_rav.eat_arm ? "CONFIRM? (item is destroyed)###rave" : "DEVOUR###rave", S(260), bh)) {
            if (!g_rav.eat_arm) g_rav.eat_arm = true; else { g_rav.eat_arm = false; rav_eat(g.handle); }
        }
        ImGui::EndDisabled();
    } else if (g_rav.offered && !call_busy(g_rav.offer_seq)) text_dim("The game offered nothing for this item.", ui::kAmber);
    text_dim("The eaten item is destroyed, like in the game. Voracity is refilled after each devour.", ui::kDim);
    show_msg("rav");
}
static void draw_gear(bool online) {
    toolbar("gear", online, call_busy(g_gear_seq), g_gfilter, sizeof g_gfilter, "Filter by tag or affix...", refresh_gear);
    show_msg("gear");
    if (!g_gear_loaded) { if (!call_busy(g_gear_seq)) text_dim(online ? "Press REFRESH to read your gear from the game." : LINK_OFF ".", ui::kDim); else loading_line("Gear"); return; }
    const std::string flt = lower(g_gfilter);
    const float avail_h = ImGui::GetContentRegionAvail().y;
    ImGui::BeginChild("##glist", ImVec2(S(330), avail_h), ImGuiChildFlags_Borders);
    int shown = 0, group = -1;
    for (auto& g : g_gear) {
        if (!gear_matches(g, flt)) continue;
        const int grp = g.equipped ? 1 : 0;
        if (grp != group) { group = grp; if (shown) ImGui::Dummy(ImVec2(0, S(4))); ui::label(g.equipped ? "Equipped" : "Bag", ui::kGold); }
        ++shown;
        ImGui::PushID((int)(g.handle & 0x7FFFFFFF));
        const std::string right = "Lv " + std::to_string(g.level);
        if (list_row("##r", g.name.empty() ? gear_name(g.tag) : g.name, right, rar_col(g.rarity), g_gsel == g.handle)) g_gsel = g.handle;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s\n%s, level %d, handle %lld", g.tag.c_str(), rar_name(g.rarity), g.level, (long long)g.handle);
        ImGui::PopID();
    }
    if (!shown) ImGui::TextDisabled(g_gear.empty() ? "No gear." : "Nothing matches the filter.");
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##gdet", ImVec2(0, avail_h), ImGuiChildFlags_Borders);
    Gear* g = find_gear(g_gsel);
    if (!g) text_dim("Select an item on the left.");
    else {
        ImGui::PushFont(g_font_bold);
        colored(g->name.empty() ? gear_name(g->tag) : g->name, rar_col(g->rarity));
        ImGui::PopFont();
        text_dim(g->tag);
        ImGui::TextDisabled("%s  |  level %d  |  %s  |  rerolls left %s  |  handle %lld", rar_name(g->rarity), g->level,
                            g->equipped ? "equipped" : "in the bag", g->rerolls < 0 ? "-" : std::to_string(g->rerolls).c_str(), (long long)g->handle);
        section_header("Affixes");
        if (g->affixes.empty()) text_dim("This item has no affixes.");
        for (int ai = 1; ai <= (int)g->affixes.size(); ++ai) draw_affix(*g, ai, online);
        text_dim("Pick an attribute and press GET THIS AFFIX (or take RANDOM OPTIONS). The affix always comes from the game's own "
                 "reroll generator; APPLY passes it back with your value / divine.");
        draw_ravenous(*g, online);
    }
    ImGui::EndChild();
}

// ---- talents (Editor > TALENTS): extra points per talent on top of what gear gives (DKT.* in dk.cpp)
struct Talent { std::string tag, name, desc; std::vector<int> floors; int total = 0, gear = 0, extra = 0; };
static std::vector<Talent> g_talents; static bool g_tal_loaded = false; static int64_t g_tal_seq = 0; static char g_tfilter[64] = {};
static void parse_talents(const json& res) {
    std::vector<Talent> v;
    for (auto& x : jlist(res)) if (x.is_object()) {
        Talent t; t.tag = jstr(x, "tag"); t.name = jstr(x, "name"); t.desc = jstr(x, "desc");
        t.total = (int)jnum(x, "total", 0); t.gear = (int)jnum(x, "gear", 0); t.extra = (int)jnum(x, "extra", 0);
        if (x.contains("floors") && x["floors"].is_array()) for (auto& f : x["floors"]) if (f.is_number()) t.floors.push_back(f.get<int>());
        if (t.name.empty()) t.name = t.tag.substr(t.tag.rfind('.') == std::string::npos ? 0 : t.tag.rfind("Talent.") == std::string::npos ? 0 : t.tag.rfind("Talent.") + 7);
        v.push_back(t);
    }
    // class talents first, then common, then wyrmling; by name inside
    auto grp = [](const Talent& t) { return t.tag.find(".Common.") != std::string::npos ? 1 : t.tag.find(".Wyrmling.") != std::string::npos ? 2 : 0; };
    std::stable_sort(v.begin(), v.end(), [&](const Talent& a, const Talent& b) { return grp(a) != grp(b) ? grp(a) < grp(b) : a.name < b.name; });
    g_talents = std::move(v); g_tal_loaded = true;
}
static void refresh_talents() {
    g_tal_seq = lua_call("DKT.list", json::array(), [](bool okv, const json& res) {
        g_tal_seq = 0;
        if (!okv) { set_msg("talents", "Refresh failed: " + err_text(res), false); return; }
        parse_talents(res); set_msg("talents", std::to_string(g_talents.size()) + " talents", true);
    });
}
static void set_talent(const std::string& tag, int extra) {
    g_tal_seq = lua_call("DKT.set", json::array({ tag, extra }), [](bool okv, const json& res) {
        g_tal_seq = 0;
        if (!okv) { set_msg("talents", "Set failed: " + err_text(res), false); return; }
        parse_talents(res);
    });
}
static int talent_rank(const Talent& t) { int r = 0; for (int f : t.floors) if (t.total >= f) ++r; return r; }
// TALENTS page: one card per talent with the game's own description visible (some talents can hurt the player when
static void draw_talents(bool online) {
    toolbar("talents", online, call_busy(g_tal_seq), g_tfilter, sizeof g_tfilter, "Filter by name or effect...", refresh_talents);
    show_msg("talents");
    if (!g_tal_loaded) {
        if (!call_busy(g_tal_seq)) { if (online) refresh_talents(); else text_dim(LINK_OFF " - load into a level with your character.", ui::kDim); }
        else loading_line("Talents");
        return;
    }
    text_dim("Read what a talent does before you raise it - some effects can hurt you. Ranks unlock at 1 / 3 / 6 points; "
             "the trainer adds points on top of what your gear gives and keeps them after zone changes.", ui::kDim);
    const std::string flt = lower(g_tfilter);
    ImGui::BeginChild("##tlist", ImVec2(0, ImGui::GetContentRegionAvail().y), ImGuiChildFlags_None);
    int group = -1;
    for (auto& t : g_talents) {
        if (!flt.empty() && lower(t.name).find(flt) == std::string::npos && lower(t.desc).find(flt) == std::string::npos) continue;
        const int grp = t.tag.find(".Common.") != std::string::npos ? 1 : t.tag.find(".Wyrmling.") != std::string::npos ? 2 : 0;
        if (grp != group) { group = grp; section_header(grp == 0 ? "Class talents" : grp == 1 ? "Common talents" : "Wyrmling talents"); }
        ImGui::PushID(t.tag.c_str());
        const int rank = talent_rank(t), nr = (int)t.floors.size();
        ImGui::PushFont(g_font_bold);
        colored(t.name, rank >= nr && nr ? ui::kGold : rank > 0 ? ui::kGreen : ui::kBone);
        ImGui::PopFont();
        ImGui::SameLine(S(250));
        ImGui::AlignTextToFramePadding();
        ImGui::TextDisabled("rank %d / %d  |  %d pts  (gear %d%s)", rank, nr, t.total, t.gear, t.extra ? (", trainer +" + std::to_string(t.extra)).c_str() : "");
        // OFF / I / II / III: the trainer's extra points needed to reach that rank's threshold on top of the gear
        static const char* roman[] = { "I", "II", "III", "IV", "V" };
        ImGui::SameLine(ImGui::GetContentRegionMax().x - S(54) * (nr + 1));
        const bool off_sel = t.extra == 0;
        if (off_sel) ImGui::BeginDisabled();
        if (ImGui::Button("OFF", ImVec2(S(50), 0)) && online) set_talent(t.tag, 0);
        if (off_sel) ImGui::EndDisabled();
        for (int k = 0; k < nr && k < 5; ++k) {
            ImGui::SameLine(0, S(4));
            const int want = std::max(0, t.floors[k] - t.gear);
            const bool cur = rank == k + 1;
            if (cur) ImGui::PushStyleColor(ImGuiCol_Button, ui::rgbv(ui::kGold, .45f));
            if (ImGui::Button(roman[k], ImVec2(S(50), 0)) && online) set_talent(t.tag, want);
            if (cur) ImGui::PopStyleColor();
            if (ImGui::IsItemHovered()) ImGui::SetTooltip("Rank %s needs %d points (gear gives %d, the trainer adds %d)", roman[k], t.floors[k], t.gear, want);
        }
        if (!t.desc.empty()) { ImGui::PushTextWrapPos(ImGui::GetContentRegionMax().x - S(8)); text_dim(t.desc, ui::kDim); ImGui::PopTextWrapPos(); }
        ImGui::Dummy(ImVec2(0, S(6)));
        ImGui::PopID();
    }
    ImGui::EndChild();
}

static void draw_editor(bool online) {
    static const char* subs[] = { "FRAGMENTS", "GEAR" };
    ui::tabs(subs, 2, &g_sub);
    if (!online) text_dim(LINK_OFF " - load into a level; the buttons work once it is running.", ui::kAmber);
    if (g_sub == 0) draw_fragments(online); else draw_gear(online);
}

} // namespace ed


// ------------------------------------------------------------------ skinned panel (frame, logo, tabs, header, rows)
static void panel_draw_skin(bool* visible, bool online, bool dev) {
    using namespace sk;
    K = 0.62f * g_ui_scale;
    const float top = L("frame_top_in_ref", 250);            // the logo stands this far above the frame
    {   const ImVec2 ds = ImGui::GetIO().DisplaySize;
        ImGui::SetNextWindowSize(ImVec2(std::min(ds.x * .92f, 1900 * K), std::min(ds.y * .95f, 1640 * K)), ImGuiCond_Once);
        ImGui::SetNextWindowPos(ImVec2(S(60), S(30)), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowSizeConstraints(ImVec2(980 * K, top + 1020 * K), ImVec2(4000, 3000)); }
    ui::theme_push();
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 0.f);
    ImGui::PushFont(g_font_body);
    const bool open = ImGui::Begin("##bl4_skin3", visible, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoBackground |
                                                           ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);
    ImGui::PopStyleVar(2);
    if (!open) { ImGui::End(); ImGui::PopFont(); ui::theme_pop(); return; }

    ImDrawList* dl = ImGui::GetWindowDrawList();
    const ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    const float fy = wp.y + top, fx1 = wp.x + ws.x, fy1 = wp.y + ws.y;
    const float band = L("band_h", 222);
    draw(dl, "frame", ImVec2(wp.x, fy), ImVec2(fx1, fy1 - band));
    draw(dl, "band", ImVec2(wp.x, fy1 - band), ImVec2(fx1, fy1));
    {   const ImVec2 s = size("lightbar"); const float x = wp.x + (ws.x - s.x) * .5f, y = fy + L("lightbar_y", 28);
        draw(dl, "lightbar", ImVec2(x, y), ImVec2(x + s.x, y + s.y)); }
    if (jnum(g_lay, "top_band", 0) != 0) {   // gold skin: logo + handles as two halves stretched beside the logo
        const float mid = wp.x + std::floor(ws.x * .5f);
        draw(dl, "top_l", ImVec2(wp.x, wp.y), ImVec2(mid, fy));
        draw(dl, "top_r", ImVec2(mid, wp.y), ImVec2(fx1, fy));
        const ImVec2 s = size("logo_low"); const float x = mid - (561 - 190) * K;
        draw(dl, "logo_low", ImVec2(x, fy), ImVec2(x + s.x, fy + s.y));
    } else {
        const ImVec2 s = size("logo"); const float x = wp.x + (ws.x - s.x) * .5f, y = fy + L("logo_y", -250);
        draw(dl, "logo", ImVec2(x, y), ImVec2(x + s.x, y + s.y)); }
    {   const ImVec2 s = size("winbtns"); const ImVec2 a(fx1 - L("winbtns_from_right", 186), fy + L("winbtns_y", 40));
        draw(dl, "winbtns", a, ImVec2(a.x + s.x, a.y + s.y));
        ImGui::SetCursorScreenPos(a);
        if (ImGui::InvisibleButton("##close", ImVec2(std::max(s.x, 1.f), std::max(s.y, 1.f)))) *visible = false;
        if (ImGui::IsItemHovered()) ImGui::SetTooltip("Hide (%s)", hotkey_name(g_settings.hotkey).c_str()); }

    const int ntab = (int)g_tabs.size(), kBridge = ntab + 1, kSettings = ntab + 2;
    std::vector<int> pages;
    for (int i = 0; i < ntab; ++i) pages.push_back(i);
    if (dev) pages.push_back(kBridge);
    pages.push_back(kSettings);
    if (g_tab == kBridge && !dev) g_tab = kSettings;
    if (std::find(pages.begin(), pages.end(), g_tab) == pages.end()) g_tab = 0;
    if (g_capturing && g_tab != kSettings) capture_cancel("Cancelled - hotkey unchanged");
    capture_poll();
    if (!ImGui::IsAnyItemActive()) {
        const int n = (int)pages.size(); int cur = 0; for (int i = 0; i < n; ++i) if (pages[i] == g_tab) cur = i;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) g_tab = pages[(cur + n - 1) % n];
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) g_tab = pages[(cur + 1) % n];
    }
    {   const float x = wp.x + L("tabs_x", 53), y0 = fy + L("tabs_y", 150);
        const float limit = fy1 - L("tabs_bottom_from_bottom", 522) - 6 * K;   // the frame's bottom-left art starts here
        const int n = (int)pages.size();
        const float tab_h = std::max(size("tab").y, 1.f), tab_w = std::max(size("tab").x, 120 * K);
        const float act_h = std::max(size("tab_active").y, 1.f), act_w = std::max(size("tab_active").x, tab_w);
        // tabs shrink to fit the rail, but never below a readable height: a rail that is still too short scrolls
        // (small screens / a small window: 17 pages used to end up a few pixels high, labels on top of each other)
        const float full_step = L("tab_step", 92), min_step = std::min(full_step, 36 * K);   // 36: the 1080p look (17 pages, no scrolling) is unchanged
        const float step = std::max(min_step, std::min(full_step, (limit - y0) / std::max(1, n)));
        const float th = std::min(tab_h, step - 6 * K), f = th / tab_h;
        const float rail_h = std::max(limit - y0, step);
        const bool scrolls = n * step > rail_h + 1.f;
        static int s_shown_tab = -1;
        ImGui::SetCursorScreenPos(ImVec2(x, y0));
        ImGui::BeginChild("##tabrail", ImVec2(act_w + (scrolls ? ImGui::GetStyle().ScrollbarSize + 2 * K : 0.f), rail_h), 0,
                          ImGuiWindowFlags_NoBackground | (scrolls ? 0 : (ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse)));
        ImDrawList* tdl = ImGui::GetWindowDrawList();
        const ImVec2 o = ImGui::GetCursorScreenPos();          // moves with the scroll position
        for (int k = 0; k < n; ++k) {
            const int i = pages[k]; const bool sel = g_tab == i;
            const std::string name = upper(i < ntab ? g_tabs[i].name : i == kBridge ? "Bridge" : "Settings");
            const ImVec2 ts = sel ? ImVec2(act_w, std::max(act_h * f, 1.f)) : ImVec2(tab_w, std::max(th, 1.f));
            const ImVec2 a(o.x, o.y + k * step + (sel ? (th - ts.y) * .5f : 0.f));
            ImGui::SetCursorScreenPos(a);
            ImGui::PushID(i);
            if (ImGui::InvisibleButton("##tab", ts)) g_tab = i;
            const bool hov = ImGui::IsItemHovered();
            if (sel && s_shown_tab != i) { s_shown_tab = i; if (scrolls) ImGui::SetScrollHereY(.5f); }   // LB / RB: keep the page's tab in view
            ImGui::PopID();
            draw(tdl, sel ? "tab_active" : "tab", a, ImVec2(a.x + ts.x, a.y + ts.y), hov && !sel ? IM_COL32(255, 255, 255, 255) : IM_COL32(236, 236, 236, 255));
            float tx = 28 * K;
            if (jnum(g_lay, "tab_icons", 0) != 0) {   // tab icon by page name (white glyph + alpha, tinted here)
                static const std::map<std::string, const char*> kIco = { {"PLAYER", "ico_player"}, {"COMBAT", "ico_weapons"}, {"LOOT", "ico_inventory"},
                    {"BUILDER", "ico_weapons"}, {"WORLD", "ico_world"}, {"MOVEMENT", "ico_teleport"}, {"DIFFICULTY", "ico_weapons"}, {"ENEMIES", "ico_player"}, {"PLACES", "ico_teleport"}, {"CAMERA", "ico_world"} };
                auto ic = kIco.find(name);
                const float isz = std::min(ts.y - 14 * K, 58 * K);
                if (ic != kIco.end() && isz > 0) draw(tdl, ic->second, ImVec2(a.x + 18 * K, a.y + (ts.y - isz) * .5f), ImVec2(a.x + 18 * K + isz, a.y + (ts.y + isz) * .5f),
                                                      sel ? rgb_(kInkT) : rgb_(0xF2E6C8));
                tx = 18 * K + std::max(isz, 0.f) + 16 * K;
            }
            text(tdl, g_font_skin, ImVec2(a.x + tx, a.y), ImVec2(a.x + ts.x - 16 * K, a.y + ts.y), rgb_(sel ? kInkT : hov ? kYellow : 0xF2E6C8), name.c_str(), 0.f, !sel);
        }
        ImGui::SetCursorScreenPos(ImVec2(o.x, o.y + n * step - 1.f)); ImGui::Dummy(ImVec2(1.f, 1.f));   // content height = every tab
        ImGui::EndChild();
    }

    // header bar: page title + search
    const float hx = wp.x + L("header_x", 330), hy = fy + L("header_y", 95), hh = size("header_bar").y;
    float hx1 = fx1 - L("header_right", 40);
    {   const std::string title = upper(g_tab < ntab ? g_tabs[g_tab].name : g_tab == kBridge ? "Bridge" : "Settings");
        const bool natural = jnum(g_lay, "header_natural", 0) != 0;   // gold: a fixed tab ("TRAINER" plate) that only grows for long titles
        const float strip = hh * (float)jnum(g_lay, "header_strip", .68);
        const float tx = natural ? L("header_text_x", 48) : 34 * K;
        if (natural) {
            ImGui::PushFont(g_font_skin); const float tw = ImGui::CalcTextSize(title.c_str()).x * .95f; ImGui::PopFont();
            hx1 = hx + std::max(size("header_bar").x, tx + tw + 165 * K);
        }
        draw(dl, "header_bar", ImVec2(hx, hy), ImVec2(hx1, hy + hh));
        text(dl, g_font_skin, ImVec2(hx + tx, hy + 1 * K), ImVec2(hx1 - (natural ? 150 : 300) * K, hy + strip), rgb_(kInkT), title.c_str(), 0.f, false, natural ? .95f : .9f);
        if (g_tab < ntab) {
            const float sw = 250 * K, sx = natural ? hx1 + 24 * K : hx1 - 270 * K - sw;
            ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(8 * K, 1));
            ImGui::SetCursorScreenPos(ImVec2(sx, hy + 2 * K + (strip - ImGui::GetFrameHeight()) * .5f));
            ImGui::SetNextItemWidth(sw);
            ImGui::PushStyleColor(ImGuiCol_FrameBg, IM_COL32(20, 18, 16, 210)); ImGui::PushStyleColor(ImGuiCol_Text, rgb_(kWhite));
            ImGui::InputTextWithHint("##search", "Search...", g_search, sizeof g_search);
            ImGui::PopStyleColor(2); ImGui::PopStyleVar();
        } }

    // content
    const float cx = wp.x + L("rows_x", 358), cy = g_lay.contains("content_bottom_from_bottom") ? fy + L("rows_y") - 6 * K : hy + hh + 22 * K, cx1 = fx1 - L("rows_right", 65);
    const float cy1 = fy1 - L("content_bottom_from_bottom", 202);                          // the content box ends above the bottom band
    ImGui::SetCursorScreenPos(ImVec2(cx, cy));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarBg, IM_COL32(0, 0, 0, 60)); ImGui::PushStyleColor(ImGuiCol_ScrollbarGrab, rgb_(kYellow, .55f));
    ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabHovered, rgb_(kYellow, .8f)); ImGui::PushStyleColor(ImGuiCol_ScrollbarGrabActive, rgb_(kYellow));
    ImGui::BeginChild("##skc", ImVec2(cx1 - cx, cy1 - cy), 0, ImGuiWindowFlags_NoBackground);
    if (g_tab < ntab) {
        if (!online) text_dim(LINK_OFF " - load into a level with your character.", ui::kAmber);
        if (g_tabs[g_tab].page == "items") items::draw_page(S(1.f));
        else if (g_tabs[g_tab].page == "missions") items::draw_missions_page(S(1.f));
        else if (g_tabs[g_tab].page == "enemies") items::draw_enemies_page(S(1.f));
        else if (g_tabs[g_tab].page == "places") items::draw_places_page(S(1.f));
        else if (g_tabs[g_tab].page == "gearmaker") items::draw_gearmaker_page(S(1.f));
        else for (auto& s : g_tabs[g_tab].sections) {
            bool any = false; for (auto& it : s.items) if (matches(it)) { any = true; break; }
            if (!any) continue;
            if (!s.name.empty()) section_header(s.name.c_str());
            for (auto& it : s.items) if (matches(it)) draw_item(it);
        }
        if (g_tabs.empty()) text_dim(g_load_msg, ui::kAmber);
    }
    else draw_settings_tab();
    ImGui::Dummy(ImVec2(0, 10 * K));
    ImGui::EndChild();
    ImGui::PopStyleColor(4);

    // status display (state + last action) and version plate, both baked into the bottom band
    {   const ImVec2 a(wp.x + L("status_text_x", 425), fy1 - L("status_text_y0", 112)), b(fx1 - L("status_text_right_from_right", 406) - 10 * K, fy1 - L("status_text_y1", 62));
        const char* t = online ? "TRAINER ACTIVE" : "WAITING FOR GAME";
        ImGui::PushFont(g_font_skin); const float tw = ImGui::CalcTextSize(t).x * 1.2f; ImGui::PopFont();
        text(dl, g_font_skin, a, b, rgb_(online ? 0x6CFF46 : kGrey), t, 0.f, true, 1.2f);
        if (!g_status.empty() && GetTickCount64() - g_status_at < 15000) {
            const ImVec2 sa(a.x + tw + 30 * K, a.y);
            dl->PushClipRect(sa, b, true);
            text(dl, g_font_body, sa, b, rgb_(g_status_ok ? 0xC9D9C0 : 0xFF6A5A), g_status.c_str(), 0.f);
            dl->PopClipRect();
        } }
    {   const ImVec2 a(fx1 - L("version_x0_from_right", 262), fy1 - L("version_y0", 106)), b(fx1 - L("version_x1_from_right", 112), fy1 - L("version_y1", 56));
        text(dl, g_font_skin, a, b, rgb_(kInkT), (std::string("V") + kVersion).c_str(), .5f, false); }

    ImGui::End();
    ImGui::PopFont();
    ui::theme_pop();
}

void panel_draw(bool* visible) {
    std::lock_guard<std::recursive_mutex> lk(g_mtx);
    g_drag_id.clear();
    const bool online = lua_online();
    const bool dev = developer_mode();
    PanelScale scale_scope(std::clamp(g_settings.ui_scale, 0.75f, 1.5f));
    if (sk::on()) { panel_draw_skin(visible, online, dev); return; }
    { const ImVec2 ds = ImGui::GetIO().DisplaySize;
      ImGui::SetNextWindowSize(ImVec2(std::max(S(980), std::min(ds.x * 0.55f, S(1700))), std::max(S(700), ds.y * 0.82f)), ImGuiCond_Once); }
    ImGui::SetNextWindowPos(ImVec2(S(60), S(70)), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowSizeConstraints(ImVec2(S(460), S(360)), ImVec2(S(2600), S(1600)));
    ui::theme_push();
    ImGui::PushFont(g_font_body);
    if (!ImGui::Begin("##tq2_bridge", visible, ImGuiWindowFlags_NoTitleBar | ImGuiWindowFlags_NoCollapse)) { ImGui::End(); ImGui::PopFont(); ui::theme_pop(); return; }

    // ---- header: two-tone title + status pills
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 wp = ImGui::GetWindowPos(), ws = ImGui::GetWindowSize();
    dl->AddRectFilledMultiColor(wp, ImVec2(wp.x + ws.x, wp.y + ws.y), ui::rgb(ui::kInk, .55f), ui::rgb(ui::kInk, .25f), ui::rgb(ui::kInk, .25f), ui::rgb(ui::kInk, .55f));
    dl->AddLine(ImVec2(wp.x, wp.y + 1), ImVec2(wp.x + ws.x, wp.y + 1), ui::rgb(ui::kGold, .8f), S(1.5f));
    ui::gold_frame(dl, ImVec2(wp.x + 1, wp.y + 1), ImVec2(wp.x + ws.x - 1, wp.y + ws.y - 1), 1.f, .5f);
    {
        std::string t1 = g_title, t2; size_t sp = g_title.find(' '); if (sp != std::string::npos) { t1 = g_title.substr(0, sp); t2 = g_title.substr(sp + 1); }
        ui::brand(t1.c_str(), t2.c_str(), "BORDERLANDS 4 TRAINER");
        ui::pill(game_base() ? "Game linked" : "Game not found", game_base() ? ui::kGreen : ui::kAmber); ImGui::SameLine(0, S(6));
        ui::pill(online ? LINK : LINK_OFF, online ? ui::kGreen : g_link.ever ? ui::kRed : ui::kAmber); ImGui::SameLine(0, S(6));
        const json& k = g_link.keeper;
        const bool ready = jbool(k, "ready", false);
        ui::pill(!online ? "Keeper ?" : ready ? "Keeper ready" : "Keeper waiting", !online ? ui::kDim : ready ? ui::kGreen : ui::kAmber); ImGui::SameLine(0, S(6));
        const std::string zone = jstr(k, "world", jstr(g_link.player, "world"));
        ui::pill(("Zone  " + (zone.empty() ? std::string("-") : zone)).c_str(), ui::kCyan);
    }
    ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - S(28));
    if (ImGui::Button("X", ImVec2(S(28), 0))) *visible = false;
    ImGui::Dummy(ImVec2(0, 2));

    const int ntab = (int)g_tabs.size(), kEditor = ntab, kBridge = ntab + 1, kSettings = ntab + 2, kTalents = ntab + 3;
    std::vector<int> pages;
    for (int i = 0; i < ntab; ++i) pages.push_back(i);
    // page replaces them (kEditor / kTalents stay defined so the rest of the panel code is unchanged).
    (void)kEditor;
    if (dev) pages.push_back(kBridge);
    pages.push_back(kSettings);
    if (g_tab == kBridge && !dev) g_tab = kSettings;
    if (std::find(pages.begin(), pages.end(), g_tab) == pages.end()) g_tab = pages.front();   // hidden pages (EDITOR / TALENTS) and stale indexes
    if (g_capturing && g_tab != kSettings) capture_cancel("Cancelled - hotkey unchanged");
    capture_poll();
    if (!ImGui::IsAnyItemActive()) {   // controller: LB / RB switch pages
        const int n = (int)pages.size();
        int cur = 0; for (int i = 0; i < n; ++i) if (pages[i] == g_tab) cur = i;
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadL1, false)) g_tab = pages[(cur + n - 1) % n];
        if (ImGui::IsKeyPressed(ImGuiKey_GamepadR1, false)) g_tab = pages[(cur + 1) % n];
    }
    const float rail = S(176.f), footer = ImGui::GetFrameHeightWithSpacing() + 6;
    ImGui::BeginChild("##rail", ImVec2(rail, -footer), ImGuiChildFlags_Borders);
    for (int i : pages) {
        const std::string name = i < ntab ? g_tabs[i].name : i == kEditor ? "Editor" : i == kTalents ? "Talents" : i == kBridge ? "Bridge" : "Settings";
        const bool sel = g_tab == i;
        if (i == ntab) { ImGui::Dummy(ImVec2(0, S(4))); ui::gold_rule(.3f); }
        ImGui::PushID(i);
        {   const ImVec2 p = ImGui::GetCursorScreenPos(); const float w = ImGui::GetContentRegionAvail().x, h = ImGui::GetFrameHeight() + S(8);
            if (ImGui::InvisibleButton("##nav", ImVec2(w, h))) g_tab = i;
            const bool hov = ImGui::IsItemHovered(); ImDrawList* nd = ImGui::GetWindowDrawList();
            if (sel) { nd->AddRectFilledMultiColor(p, ImVec2(p.x + w, p.y + h), ui::rgb(ui::kGold, .22f), ui::rgb(ui::kGold, .02f), ui::rgb(ui::kGold, .02f), ui::rgb(ui::kGold, .22f));
                       nd->AddRectFilled(p, ImVec2(p.x + S(3), p.y + h), ui::rgb(ui::kGold));
                       ui::gold_frame(nd, p, ImVec2(p.x + w - 1, p.y + h - 1), 1.f, .55f); }
            else if (hov) nd->AddRectFilled(p, ImVec2(p.x + w, p.y + h), ui::rgb(0xFFFFFF, .04f));
            std::string up = name; std::transform(up.begin(), up.end(), up.begin(), ::toupper);
            const ImVec2 ts = ImGui::CalcTextSize(up.c_str());
            nd->AddText(ImVec2(p.x + S(14), p.y + (h - ts.y) * .5f), sel ? ui::rgb(ui::kGoldHi) : hov ? ui::rgb(ui::kBone) : ui::rgb(ui::kDim), up.c_str()); }
        ImGui::PopID();
    }
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##content", ImVec2(0, -footer), ImGuiChildFlags_Borders);
    if (g_tab < ntab) {
        ImGui::SetNextItemWidth(-1);
        ImGui::InputTextWithHint("##search", "Search...", g_search, sizeof g_search);
        ui::page_title(g_tabs[g_tab].name.c_str());
        if (!online) {
            text_dim(LINK_OFF " - load into a level with your character.", ui::kAmber);
        }
        if (g_tabs[g_tab].page == "items") items::draw_page(S(1.f));
        else if (g_tabs[g_tab].page == "missions") items::draw_missions_page(S(1.f));
        else if (g_tabs[g_tab].page == "enemies") items::draw_enemies_page(S(1.f));
        else if (g_tabs[g_tab].page == "places") items::draw_places_page(S(1.f));
        else if (g_tabs[g_tab].page == "gearmaker") items::draw_gearmaker_page(S(1.f));
        else for (auto& s : g_tabs[g_tab].sections) {
            bool any = false; for (auto& it : s.items) if (matches(it)) { any = true; break; }
            if (!any) continue;
            if (!s.name.empty()) section_header(s.name.c_str());
            for (auto& it : s.items) if (matches(it)) draw_item(it);
        }
        if (g_tabs.empty()) text_dim(g_load_msg, ui::kAmber);
    } else if (g_tab == kEditor) {
        ui::page_title("Editor", "Fragments and gear, rerolled with the game's own rules");
        ed::draw_editor(online);
    } else if (g_tab == kTalents) {
        ui::page_title("Talents", "Raise any talent on its own, on top of what your gear gives");
        ed::draw_talents(online);
    }
    else {
        ui::page_title("Settings");
        draw_settings_tab();
    }
    ImGui::EndChild();

    // ---- footer
    auto dot = [](uint32_t col) {
        ImVec2 p = ImGui::GetCursorScreenPos(); float h = ImGui::GetFrameHeight();
        ImGui::GetWindowDrawList()->AddCircleFilled(ImVec2(p.x + 5, p.y + h * 0.5f), 4.f, ui::rgb(col));
        ImGui::Dummy(ImVec2(12, h)); ImGui::SameLine();
    };
    ImGui::AlignTextToFramePadding();
    dot(online ? ui::kGreen : ui::kAmber); ImGui::TextDisabled(online ? LINK_FOOT : LINK_FOOT_OFF); ImGui::SameLine(0, 18);
    if (!g_status.empty() && GetTickCount64() - g_status_at < 15000) {
        ImGui::PushStyleColor(ImGuiCol_Text, ui::rgbv(g_status_ok ? ui::kBone : ui::kRed));
        std::string st = g_status; if (st.size() > 90) st = st.substr(0, 87) + "...";
        ImGui::TextUnformatted(st.c_str());
        ImGui::PopStyleColor();
        ImGui::SameLine();
    }
    {   const std::string hide = hotkey_name(g_settings.hotkey) + "  hide";
        ImGui::SameLine(ImGui::GetWindowContentRegionMax().x - ImGui::CalcTextSize(hide.c_str()).x - S(4));
        ImGui::TextDisabled("%s", hide.c_str()); }
    ImGui::End();
    ImGui::PopFont();
    ui::theme_pop();
}


} // namespace wb
