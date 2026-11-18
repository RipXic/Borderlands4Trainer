// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
//
//   BL.STATE                    one-call game state: solo check, map, player readouts (api.json "readouts")
//   BL.CALL {name, args, v}     run a named action from api.json (the game's own function on the right object)
//   BL.API                      list actions + readouts
//   BL.TOGGLE {id, v} / BL.TOGGLES   keeper toggles (re-applied on a new pawn, ticked while solo)
//   BL.TARGET {target}          resolve a target (debugging)
//   BL.POS / BL.TELEPORT {x,y,z}     player position / move (Actor K2_GetActorLocation / K2_SetActorLocation)
//   BL.ENUM {name}              enum names + values
//   BL.API_RELOAD               re-read api.json (also automatic when the file changes)
// game functions become callable WITHOUT a rebuild. Functions are found by REFLECTED NAME (survives patches).
// Every write is refused unless the session is standalone (solo/offline): Borderlands 4 has online co-op.
#include <array>
#include <functional>
#include "dk.h"
#include "ue.h"
#include <atomic>
#include <Xinput.h>
#include "pad_share.h"
#include <deque>
#include <fstream>
#include <map>
#include <unordered_map>
#include <set>
#include <algorithm>
#include <cmath>
#include <mutex>

namespace dk {
using namespace ::wb;

static std::mutex g_mtx;
static json g_results = json::object();
static std::deque<uint64_t> g_result_order;
static std::atomic<uint64_t> g_ver{1};
static uint64_t g_seq_done = 0;
static std::string g_data_dir, g_spec, g_api_path;
static json g_api = json::object();
static FILETIME g_api_time{};
static json g_spec_json = json::object();
static json g_log = json::array();

static void plog(const std::string& s) {
    std::lock_guard<std::mutex> lk(g_mtx);
    g_log.push_back(s); while (g_log.size() > 30) g_log.erase(g_log.begin());
    ::wb::log("[bl4] %s", s.c_str());
}

// ------------------------------------------------------------------ files
static bool ftime(const std::string& p, FILETIME& t) {
    WIN32_FILE_ATTRIBUTE_DATA d; if (!GetFileAttributesExA(p.c_str(), GetFileExInfoStandard, &d)) return false;
    t = d.ftLastWriteTime; return true;
}
static json read_json(const std::string& p) {
    std::ifstream f(p); if (!f) return json();
    try { return json::parse(f, nullptr, true, true); } catch (...) { return json(); }
}
static unsigned g_api_gen = 0;   // bumped on every api.json load (compiled tables such as the hotkeys rebuild)
static void load_api(bool force) {
    FILETIME t{}; if (!ftime(g_api_path, t)) return;
    if (!force && CompareFileTime(&t, &g_api_time) == 0) return;
    json j = read_json(g_api_path);
    if (!j.is_object()) { plog("api.json did not parse - keeping the old table"); g_api_time = t; return; }
    g_api = j; g_api_time = t; ++g_api_gen;
    plog("api.json loaded: " + std::to_string(j.value("actions", json::object()).size()) + " actions, " +
         std::to_string(j.value("readouts", json::object()).size()) + " readouts");
}
// g_spec_json / g_active are written on the game thread only, but state() reads them on the panel thread: writes hold g_mtx.
static void load_spec() { json j = read_json(g_spec); if (j.is_object()) { std::lock_guard<std::mutex> lk(g_mtx); g_spec_json = std::move(j); } }
// a table of api.json by reference (GAME THREAD; no deep copy every tick). Not an object / missing = empty table.
static const json& api_tbl(const char* k) {
    static const json empty = json::object();
    if (!g_api.is_object()) return empty;
    auto it = g_api.find(k); return it != g_api.end() && it->is_object() ? *it : empty;
}

// ------------------------------------------------------------------ targets (GAME THREAD)
static uintptr_t prop_obj(uintptr_t o, const char* field) {
    if (!o) return 0;
    uintptr_t base = 0; ue::Prop leaf; if (!ue::resolve(o, ue::class_of(o), field, base, leaf)) return 0;
    return ue::get<uintptr_t>(base, leaf);
}
// Lookups that scan the object array are expensive (~480k objects): results are cached and only re-validated
// (alive + same class) on later calls. The cache is dropped when the player character changes (zone change / load).
static uintptr_t g_pc_cache = 0, g_pc_cls = 0;
static ULONGLONG g_pc_scan_at = 0;
static bool still_valid(uintptr_t o, uintptr_t cls) { return o && ue::alive(o) && ue::class_of(o) == cls; }
// Objects of one class without a frame hitch: walks the object array in slices (12k slots per call, ~1.5 ms) and publishes
// the list each completed pass (a full find_all on a timer froze the game). Stepped by scans_tick() (keeper) only while a
// job asked for it: want(true) = a slice every tick (~0.6 s per pass), want(false) = 4 slices a second (territory learning).
// The list can be up to a pass old: live() drops objects that died / whose address was reused by another class.
struct SliceScan {
    const char* cls_name; uintptr_t cls = 0; int32_t pos = 0; std::vector<uintptr_t> cur, list; uint64_t passes = 0;
    ULONGLONG fast_until = 0, slow_until = 0, slow_at = 0;
    explicit SliceScan(const char* c) : cls_name(c) {}
    bool step(int32_t budget = 12000) {
        if (!cls || !ue::alive(cls)) { cls = ue::find_class(cls_name); if (!cls) return false; }
        const int32_t n = ue::num_objects(); const int32_t end = std::min(n, pos + budget);
        for (int32_t i = pos; i < end; ++i) { const uintptr_t o = ue::object_at(i); if (!o || !ue::is_child_of(ue::class_of(o), cls) || ue::is_cdo(o)) continue; cur.push_back(o); }
        pos = end;
        if (pos >= n) { list.swap(cur); cur.clear(); pos = 0; ++passes; return true; }
        return false;
    }
    void want(bool fast = true) { const ULONGLONG t = GetTickCount64() + 3000; if (fast) fast_until = t; else slow_until = t; }
    void tick(ULONGLONG now) { if (now < fast_until) step(); else if (now < slow_until && now - slow_at >= 250) { slow_at = now; step(); } }
    std::vector<uintptr_t> live() const {
        std::vector<uintptr_t> out; if (!cls) return out; out.reserve(list.size());
        for (uintptr_t o : list) if (ue::alive(o) && ue::is_child_of(ue::class_of(o), cls)) out.push_back(o);
        return out;
    }
};
static SliceScan g_pick_scan("InventoryPickup"), g_char_scan("OakCharacter");
static void scans_tick() { const ULONGLONG now = GetTickCount64(); g_char_scan.tick(now); g_pick_scan.tick(now); }
static uintptr_t player_pc() {
    if (still_valid(g_pc_cache, g_pc_cls) && prop_obj(g_pc_cache, "Player")) return g_pc_cache;
    const ULONGLONG now = GetTickCount64();
    if (now - g_pc_scan_at < 1000) return 0;            // no controller (main menu): rescan at most once a second
    g_pc_scan_at = now;
    // the local player controller is the one with a Player (ULocalPlayer)
    for (uintptr_t pc : ue::find_all(ue::find_class("PlayerController"), false, 16))
        if (prop_obj(pc, "Player")) { g_pc_cache = pc; g_pc_cls = ue::class_of(pc); return pc; }
    g_pc_cache = 0; return 0;
}
// The player's CHARACTER, also while driving (the controller's Pawn is then the OakVehicle - skill points, SDU, jump,
// attributes and toggles all belong to the character). OakPlayerController.OakPlayerCharacter, else the Pawn.
static uintptr_t player_controlled() { return prop_obj(player_pc(), "Pawn"); }   // what the player steers (vehicle or character)
static uintptr_t player_pawn() {
    const uintptr_t pc = player_pc(); if (!pc) return 0;
    static uintptr_t cls = 0; static int32_t off = -1;
    const uintptr_t c = ue::class_of(pc);
    if (c != cls) { cls = c; ue::Prop p = ue::find_prop(c, "OakPlayerCharacter"); off = (p && p.size == 8) ? p.offset : -1; }
    uintptr_t ch = 0; if (off >= 0) ue::rd_as(pc + off, ch);
    return (ch && ue::alive(ch)) ? ch : prop_obj(pc, "Pawn");
}
static uintptr_t player_ps() { return prop_obj(player_pc(), "PlayerState"); }
struct Cached { uintptr_t obj = 0, cls = 0; };
static std::map<std::string, Cached> g_owned_cache;
static uintptr_t g_owned_pawn = 0;
static std::map<std::string, ULONGLONG> g_owned_miss;   // lookups that found nothing (no full object walk on every call)
static uintptr_t owned_by(const std::string& cls, uintptr_t owner, const std::string& name = "") {
    if (!owner) return 0;
    const uintptr_t pawn = player_pawn();
    if (pawn != g_owned_pawn) { g_owned_cache.clear(); g_owned_miss.clear(); g_owned_pawn = pawn; }
    const std::string key = hexstr(owner) + "|" + cls + "|" + name;
    auto it = g_owned_cache.find(key);
    if (it != g_owned_cache.end() && still_valid(it->second.obj, it->second.cls) && ue::outer_of(it->second.obj) == owner) return it->second.obj;
    const ULONGLONG now_ms = GetTickCount64();
    { auto ms = g_owned_miss.find(key); if (ms != g_owned_miss.end() && now_ms - ms->second < 3000) return 0; }   // not there a moment ago
    const uintptr_t c = ue::find_class(cls); if (!c) return 0;
    for (uintptr_t o : ue::find_all(c, false, 0))
        if (ue::outer_of(o) == owner && (name.empty() || ue::name_of(o) == name)) { g_owned_cache[key] = { o, ue::class_of(o) }; g_owned_miss.erase(key); return o; }
    if (g_owned_miss.size() > 500) g_owned_miss.clear();
    g_owned_miss[key] = now_ms;
    return 0;
}
// "player.pc" | "player.pawn" | "player.ps" | "player.comp:<Class>" | "player.ps.comp:<Class>[/<Name>]" |
// "cdo:<Class>" | "first:<Class>" | "addr:<expr-free hex>" | "obj:<path>"
static uintptr_t target(const std::string& t, std::string* why = nullptr) {
    auto W = [&](const char* s) { if (why) *why = s; return (uintptr_t)0; };
    if (t == "player.pc") { uintptr_t v = player_pc(); return v ? v : W("no player controller (main menu?)"); }
    if (t == "player.pawn") { uintptr_t v = player_pawn(); return v ? v : W("no player character (main menu / loading?)"); }
    if (t.rfind("player.comp:", 0) == 0) {
        uintptr_t pawn = player_pawn(); if (!pawn) return W("no player character");
        uintptr_t v = owned_by(t.substr(12), pawn); return v ? v : W("component not on the player character");
    }
    if (t == "player.ps") { uintptr_t v = player_ps(); return v ? v : W("no player state"); }
    if (t.rfind("player.ps.comp:", 0) == 0) {   // player.ps.comp:<Class>[/<ObjectName>]
        uintptr_t ps = player_ps(); if (!ps) return W("no player state");
        std::string s = t.substr(15), nm; const size_t k = s.find('/'); if (k != std::string::npos) { nm = s.substr(k + 1); s = s.substr(0, k); }
        uintptr_t v = owned_by(s, ps, nm); return v ? v : W("object not on the player state");
    }
    if (t.rfind("cdo:", 0) == 0) { uintptr_t v = ue::cdo_of(ue::find_class(t.substr(4))); return v ? v : W("class not found"); }
    if (t.rfind("first:", 0) == 0) { uintptr_t v = ue::find_first(ue::find_class(t.substr(6))); return v ? v : W("no live instance"); }
    if (t.rfind("obj:", 0) == 0) { uintptr_t v = ue::find_object(t.substr(4)); return v ? v : W("object not found"); }
    if (t.rfind("addr:", 0) == 0) { uintptr_t v = std::stoull(t.substr(5), nullptr, 16); return ue::alive(v) ? v : W("not a live object"); }
    return W("unknown target");
}

// ------------------------------------------------------------------ session (solo guard)
static json session_uncached();
// session() walked every object (PlayerState count) = ~27 ms on the game thread; the toggles keeper, attrs and many
// commands call it -> visible stutter. Cached per player character for 5 s.
static json session() {
    static json cache; static uintptr_t cache_pawn = 0; static ULONGLONG cache_at = 0; static std::mutex mtx;
    const uintptr_t pawn = player_pawn(); const ULONGLONG now = GetTickCount64();
    { std::lock_guard<std::mutex> lk(mtx); if (pawn && pawn == cache_pawn && now - cache_at < 5000 && cache.is_object()) return cache; }
    json s = session_uncached();
    { std::lock_guard<std::mutex> lk(mtx); cache = s; cache_pawn = pawn; cache_at = now; }
    return s;
}
static json session_uncached() {
    json s = { {"in_game", false}, {"standalone", false} };
    uintptr_t pawn = player_pawn(); if (!pawn) return s;
    s["in_game"] = true;
    static uintptr_t ksl = 0; if (!ksl || !ue::alive(ksl)) ksl = ue::cdo_of(ue::find_class("KismetSystemLibrary"));
    json r = ue::call_json(ksl, "IsStandalone", json::object(), pawn);
    const bool standalone = r.value("ok", false) && r["data"]["ReturnValue"].value("value", 0.0) != 0;
    json rs = ue::call_json(ksl, "IsServer", json::object(), pawn);
    const bool server = rs.value("ok", false) && rs["data"]["ReturnValue"].value("value", 0.0) != 0;
    // Borderlands 4 hosts every session as a listen server (co-op ready) even when you play alone: count the players.
    int players = 0;   // GameplayStatics::GetNumPlayerStates (the old find_all(PlayerState) walked every object: ~30 ms)
    { static uintptr_t gsl = 0; if (!gsl || !ue::alive(gsl)) gsl = ue::cdo_of(ue::find_class("GameplayStatics"));
      json np = ue::call_json(gsl, "GetNumPlayerStates", { {"WorldContextObject", "addr:" + hexstr(pawn)} }, 0);
      if (np.value("ok", false)) players = (int)np["data"]["ReturnValue"].value("value", 0.0);
      else for (uintptr_t ps : ue::find_all(ue::find_class("PlayerState"), false, 16)) if (ue::outer_of(ps) == ue::outer_of(pawn)) ++players; }
    s["net"] = standalone ? "standalone" : (server ? "listen_server" : "client");
    s["players"] = players;
    // "standalone" = safe to change things: a real standalone game, or a host session with nobody else in it.
    // A client (joined someone else's game) or a host with other players is never solo.
    s["standalone"] = standalone || (server && players == 1);
    // map = the world the pawn lives in (pawn -> PersistentLevel -> World)
    s["map"] = ue::name_of(ue::outer_of(ue::outer_of(pawn)));
    return s;
}

// ------------------------------------------------------------------ readouts / actions (GAME THREAD)
// a currency balance by name ("Cash", "eridium", "golden_key", "VaultCard01_Tokens"...): the player controller's
// GbxCurrencyManager.currencies = TArray<GbxCurrency {SToken type {Hash, Name}, UInt64 Amount @0x10}>
static json currency_amount(const std::string& cur) {
    uintptr_t pc = player_pc(); if (!pc) return json{ {"error", "no player controller"} };
    uintptr_t mgr = owned_by("GbxCurrencyManager", pc); if (!mgr) return json{ {"error", "no GbxCurrencyManager"} };
    ue::Prop arr = ue::find_prop(ue::class_of(mgr), "currencies"); if (!arr) return json{ {"error", "no currencies field"} };
    ue::Prop inner = ue::array_inner(arr); const int stride = inner ? inner.size : 0x18;
    auto a = ue::array_of(mgr, arr);
    for (int32_t i = 0; i < a.num; ++i) {
        const uintptr_t e = (uintptr_t)a.data + (uintptr_t)i * stride;
        ue::FName n{}; ue::rd_as(e + 4, n);
        if (_stricmp(ue::name_str(n).c_str(), cur.c_str()) == 0) { uint64_t v = 0; ue::rd_as(e + 0x10, v); return (double)v; }
    }
    return json{ {"error", "currency not found: " + cur} };
}

static json attr_get(const std::string& n, uintptr_t on);
static std::string diff_label();
static std::string diff_stats_text();
static void diff_tick();
static void enemy_tick();
static void camera_tick();
static void tp_idle();
static json points_cmd(const json& a);
static json readout(const json& r) {
    if (r.contains("currency")) return currency_amount(r["currency"].get<std::string>());
    if (r.contains("points") && r["points"].is_string()) {   // skill_points / spec_tokens / sdu_tokens earned (BL.POINTS)
        json pr = points_cmd(json::object());
        if (!pr.value("ok", false)) return json{ {"error", pr.value("message", std::string("failed"))} };
        return pr["data"].value(r["points"].get<std::string>(), json{ {"error", "no such pool"} });
    }
    if (r.contains("attr")) return attr_get(r["attr"].get<std::string>(), 0);
    std::string why; uintptr_t o = target(r.value("target", ""), &why);
    if (!o) return json{ {"error", why} };
    if (r.contains("func")) {
        json merged = r.value("args", json::object());
        for (auto& kv : merged.items())   // object arguments: "target:player.pawn" -> "addr:<hex>"
            if (kv.value().is_string() && kv.value().get<std::string>().rfind("target:", 0) == 0) {
                uintptr_t t = target(kv.value().get<std::string>().substr(7), &why);
                if (!t) return json{ {"error", kv.key() + ": " + why} };
                kv.value() = "addr:" + hexstr(t);
            }
        json res = ue::call_json(o, r["func"].get<std::string>(), merged, player_pawn());
        if (!res.value("ok", false)) return json{ {"error", res.value("message", std::string("call failed"))} };
        const json& d = res["data"];
        if (d.contains("ReturnValue")) { const json& v = d["ReturnValue"]; return v.contains("value") ? v["value"] : v; }
        return d;
    }
    if (r.contains("field")) {
        uintptr_t base = 0; ue::Prop leaf;
        if (!ue::resolve(o, ue::class_of(o), r["field"].get<std::string>(), base, leaf)) return json{ {"error", "field not found"} };
        return ue::get_num(base, leaf);
    }
    return json{ {"error", "readout needs func or field"} };
}

static json state_now() {
    json s = session();
    json ro = json::object();
    if (s.value("in_game", false))
    { const json& ros = api_tbl("readouts");
        for (auto& kv : ros.items()) ro[kv.key()] = readout(kv.value()); }
    s["player"] = ro;
    return s;
}

// numeric write on a field; a GameplayAttributeData field gets BaseValue + CurrentValue (GAS keeps both)
static bool write_field(uintptr_t o, const std::string& field, double v, std::string* why) {
    uintptr_t base = 0; ue::Prop leaf;
    if (!ue::resolve(o, ue::class_of(o), field, base, leaf)) { if (why) *why = "field not found: " + field; return false; }
    if (leaf.type == "StructProperty") {
        const std::string sn = ue::name_of(leaf.owner_struct);
        // Gearbox attribute {base4, Value @4, BaseValue @8} (GbxAttributeFloat / GbxAttributeInteger): set both
        if (sn == "GbxAttributeFloat") { const float f = (float)v; ue::mwrite(base + leaf.offset + 4, &f, 4); ue::mwrite(base + leaf.offset + 8, &f, 4); return true; }
        if (sn == "GbxAttributeInteger") { const int32_t i = (int32_t)llround(v); ue::mwrite(base + leaf.offset + 4, &i, 4); ue::mwrite(base + leaf.offset + 8, &i, 4); return true; }
        if (sn.find("AttributeData") == std::string::npos) { if (why) *why = "struct field (not an attribute)"; return false; }
        const float f = (float)v; ue::mwrite(base + leaf.offset + 8, &f, 4); ue::mwrite(base + leaf.offset + 0xC, &f, 4); return true;
    }
    if (leaf.type == "BoolProperty") return ue::set_bool(base, leaf, v != 0);
    return ue::set_num(base, leaf, v);
}

// prepare args: merge, fill v, resolve "target:<t>" and "$r:<readout>"
static bool prep_args(const json& a, const json& args, const json& v, const std::string& name, json& merged, json& err) {
    merged = a.value("args", json::object());
    if (args.is_object()) for (auto& kv : args.items()) merged[kv.key()] = kv.value();
    if (!v.is_null() && a.contains("arg")) merged[a["arg"].get<std::string>()] = v;
    const json& ros = api_tbl("readouts");
    // resolves "$v" (missing), "target:<t>", "$r:<readout>" and "$transform:<readout>" anywhere (also inside structs)
    std::function<bool(const std::string&, json&)> fix = [&](const std::string& key, json& val) -> bool {
        if (val.is_object()) { for (auto& kv : val.items()) if (!fix(kv.key(), kv.value())) return false; return true; }
        if (!val.is_string()) return true;
        const std::string s = val.get<std::string>();
        if (s == "$v") { err = fail("NEEDS_VALUE", name + " needs a value (v)"); return false; }
        if (s.rfind("target:", 0) == 0) {   // another target as an object argument, e.g. "target:player.pc"
            std::string why; uintptr_t t = target(s.substr(7), &why);
            if (!t) { err = fail("NO_TARGET", key + " = " + s + ": " + why); return false; }
            val = "addr:" + hexstr(t);
        } else if (s.rfind("$r:", 0) == 0 || s.rfind("$transform:", 0) == 0) {  // live readout ("$r:max_health")
            const bool tr = s[1] == 't'; const std::string rn = s.substr(tr ? 11 : 3);
            if (!ros.contains(rn)) { err = fail("NO_READOUT", rn); return false; }
            json rv = readout(ros[rn]);
            if (!rv.is_number() && !(rv.is_object() && !rv.contains("error"))) { err = fail("READOUT", rn + ": " + rv.dump()); return false; }
            // "$transform:position" = an FTransform at that location (identity rotation, scale 1)
            val = tr ? json{ {"Rotation", { {"W", 1.0} }}, {"Translation", rv}, {"Scale3D", { {"X", 1.0}, {"Y", 1.0}, {"Z", 1.0} }} } : rv;
        }
        return true;
    };
    for (auto& kv : merged.items()) if (!fix(kv.key(), kv.value())) return false;
    return true;
}

// run one action spec {target, func|field|fields, args, arg, after, write}
static json exec_spec(const json& a, const json& args, const json& v, const std::string& name, bool quiet = false) {
    json merged, err;
    if (!prep_args(a, args, v, name, merged, err)) return err;
    if (a.value("write", true)) {
        json s = session();
        if (!s.value("in_game", false)) return fail("NOT_IN_GAME", "load a character first");
        if (!s.value("standalone", false)) return fail("NOT_SOLO", "refused: not a solo/offline session");
    }
    std::string why; uintptr_t o = target(a.value("target", ""), &why);
    if (!o) return fail("NO_TARGET", a.value("target", "") + ": " + why);
    json r;
    if (a.contains("field") || a.contains("fields")) {   // field write: value = v, or "value" in the spec
        json val = !v.is_null() ? v : a.value("value", json());
        if (val.is_string()) { json tmp = { {"x", val} }; json m2, e2; json spec = { {"args", tmp} };
            if (!prep_args(spec, json::object(), json(), name, m2, e2)) return e2; val = m2["x"]; }
        if (!val.is_number() && !val.is_boolean()) return fail("NEEDS_VALUE", name + " needs a number (v)");
        const double d = val.is_boolean() ? (val.get<bool>() ? 1 : 0) : val.get<double>();
        json fields = a.contains("fields") ? a["fields"] : json::array({ a["field"] });
        for (auto& f : fields) { std::string w; if (!write_field(o, f.get<std::string>(), d, &w)) return fail("WRITE", w); }
        r = ok({ {"set", fields}, {"value", d}, {"on", ue::full_name(o)} });
    } else {
        ue::journal_scope(true);
        r = ue::call_json(o, a.value("func", ""), merged, player_pawn());
        ue::journal_scope(false);
    }
    if (!quiet) plog(name + " " + merged.dump() + " -> " + (r.value("ok", false) ? "ok" : r.value("message", std::string("failed"))));
    if (r.value("ok", false) && a.contains("after")) {   // optional readout to show the effect: "after": "<readout name>"
        const json& ros = api_tbl("readouts");
        const std::string rn = a["after"].get<std::string>();
        if (ros.contains(rn)) r["data"]["after"] = { {rn, readout(ros[rn])} };
    }
    return r;
}

static json run_action(const std::string& name, json args, const json& v) {
    const json& acts = api_tbl("actions");
    if (!acts.contains(name)) return fail("NO_ACTION", "unknown action '" + name + "' (BL.API lists them)");
    return exec_spec(acts[name], args, v, name);
}

// ------------------------------------------------------------------ toggles (keeper)
// api.json "toggles": { id: { desc, on: <spec>, off: <spec>, tick: <spec>, every_ms, value: default, arg } }
// on/off/tick are action specs or {"action": "<name>", "v": ...}. Active toggles + values live in <data>\toggles.json,
// are re-applied ("on") whenever the player character changes (zone change / load) and ticked while in a solo game.
static json g_active = json::object();          // id -> value (true or number)
static uintptr_t g_last_pawn = 0;
static std::string toggles_file() { return g_data_dir + "\\toggles.json"; }
static void save_toggles() {
    // create the folder chain (data dir may be <proj>\bin\native with no <proj>\bin yet)
    for (size_t k = g_data_dir.find('\\', 3); ; k = g_data_dir.find('\\', k + 1)) {
        CreateDirectoryA(g_data_dir.substr(0, k).c_str(), nullptr); if (k == std::string::npos) break;
    }
    std::ofstream f(toggles_file()); f << g_active.dump(1);
}
static json tq_exec(const std::string& name, const json& a);
static json run_part(const json& part, const json& v, const std::string& name, bool quiet) {
    if (part.is_null()) return ok(nullptr);
    if (part.contains("cmd") && part["cmd"].is_string()) {   // a trainer command: {"cmd": "BL.ATTR", "args": {...}}
        std::string c = part["cmd"].get<std::string>(); if (c.rfind("BL.", 0) == 0) c = "TQ." + c.substr(3);
        json args = part.value("args", json::object()); if (!v.is_null() && !args.contains("v")) args["v"] = v;   // the toggle's value
        return tq_exec(c, args);
    }
    if (part.contains("action")) {
        const json& acts = api_tbl("actions"); const std::string an = part["action"].get<std::string>();
        if (!acts.contains(an)) return fail("NO_ACTION", an);
        return exec_spec(acts[an], part.value("args", json::object()), part.contains("v") ? part["v"] : v, an, quiet);
    }
    return exec_spec(part, json::object(), part.contains("v") ? part["v"] : v, name, quiet);
}
static json set_toggle(const std::string& id, const json& val) {
    const json& tg = api_tbl("toggles");
    if (!tg.contains(id)) return fail("NO_TOGGLE", id);
    const json& t = tg[id];
    const bool on = val.is_boolean() ? val.get<bool>() : (val.is_number() ? true : false);
    json r;
    if (on) {
        json v = val.is_number() ? val : t.value("value", json());
        r = run_part(t.value("on", json()), v, id + ".on", false);
        if (!r.value("ok", false)) return r;
        { std::lock_guard<std::mutex> lk(g_mtx); g_active[id] = val.is_number() ? val : json(true); }
    } else {
        r = run_part(t.value("off", json()), json(), id + ".off", false);
        { std::lock_guard<std::mutex> lk(g_mtx); g_active.erase(id); }
    }
    save_toggles();
    return r;
}
static json g_ro_cache = json::object();       // panel readouts, refreshed on the game thread every 500 ms
struct PerfStat { double total_ms = 0, max_ms = 0; uint64_t n = 0; };
static std::map<std::string, PerfStat> g_perf; static std::mutex g_perf_mtx;
struct PerfScope { template <class T> explicit PerfScope(T&&) {} };
static void refresh_readouts() {
    static ULONGLONG last = 0; const ULONGLONG now = GetTickCount64();
    if (now - last < 1000) return; last = now;
    json out = json::object();
    const json& ros = api_tbl("readouts");
    const json tabs = g_spec_json.value("tabs", json::array());
    const bool ing = player_pawn() != 0;
    for (auto& tb : tabs) { const json secs = tb.value("sections", json::array());
        for (auto& s : secs) { const json items = s.value("items", json::array());
            for (auto& it : items) {
                if (!it.contains("readout")) continue;
                const std::string rn = it["readout"].get<std::string>();
                PerfScope ps_ro("ro:" + rn);
                json v = rn == "difficulty" ? json(diff_label()) : rn == "difficulty_stats" ? json(diff_stats_text())
                       : (ing && ros.contains(rn)) ? readout(ros[rn]) : json("-");
                if (v.is_number()) { const double d = v.get<double>(); char b[64];   // snprintf truncates; sprintf_s ends the process on a huge value
                    if (std::fabs(d) < 9e15 && d == (double)(long long)d) snprintf(b, sizeof b, "%lld", (long long)d); else snprintf(b, sizeof b, "%.2f", d); v = b; }
                else if (v.is_object() && v.contains("X")) { char b[96]; snprintf(b, sizeof b, "%.0f, %.0f, %.0f", v.value("X", 0.0), v.value("Y", 0.0), v.value("Z", 0.0)); v = b; }
                else if (!v.is_string()) v = v.dump();
                out[it.value("id", "")] = v;
            } } }
    std::lock_guard<std::mutex> lk(g_mtx); g_ro_cache = out; ++g_ver;
}
static void loot_tick();
static void hud_tick();
static void build_tick();
static void attrs_tick();
static json session();
// BL.PERF: time spent per job on the game thread (each call of the 15 ms tick)

// Performance test: pause single trainer jobs at run time (BL.PERF {toggle|off|on: name | "all"}) to find a slowdown.
static std::set<std::string> g_job_off; static std::mutex g_job_mtx;
static bool job_on(const std::string& k) { std::lock_guard<std::mutex> lk(g_job_mtx); return !g_job_off.count(k) && !g_job_off.count("all"); }
static const char* kJobs[] = { "refresh_readouts", "loot_tick", "pts_tick", "hud_tick", "build_tick", "diff_tick", "enemy_tick", "camera_tick", "attrs_tick", "toggles", "hotkeys_tick", "file_watch" };
// BL.FRAMES: the engine's own frame time (GameplayStatics::GetWorldDeltaSeconds, real seconds = delta / time dilation
// is not needed: we only compare runs) sampled every tick -> how even the frames are.
// Ring buffer: the newest kFramesCap samples (it used to stop recording when full and then reported old data).
static std::vector<float> g_frames; static size_t g_frames_head = 0; static std::mutex g_frames_mtx;
static const size_t kFramesCap = 20000;
static void frames_sample() {
    static uintptr_t gs = 0; if (!gs || !ue::alive(gs)) gs = ue::cdo_of(ue::find_class("GameplayStatics"));
    const uintptr_t pawn = player_pawn(); if (!gs || !pawn) return;
    ue::Params p = ue::params_for(gs, "GetWorldDeltaSeconds"); if (!p) return;
    p.put("WorldContextObject", pawn);
    if (!ue::call(gs, p)) return;
    const double d = p.ret<double>() * 1000.0;
    std::lock_guard<std::mutex> lk(g_frames_mtx);
    if (g_frames.size() < kFramesCap) g_frames.push_back((float)d);
    else { g_frames[g_frames_head] = (float)d; g_frames_head = (g_frames_head + 1) % kFramesCap; }
}
static json frames_cmd(const json& a) {
    std::vector<float> v;
    { std::lock_guard<std::mutex> lk(g_frames_mtx); const size_t n = g_frames.size(); v.reserve(n);
      for (size_t i = 0; i < n; ++i) v.push_back(g_frames[(g_frames_head + i) % n]);   // oldest first
      if (a.value("reset", false)) { g_frames.clear(); g_frames_head = 0; } }
    if (v.empty()) return ok({ {"n", 0} });
    std::vector<float> s2 = v; std::sort(s2.begin(), s2.end());
    auto pct = [&](double q) { return s2[std::min(s2.size() - 1, (size_t)(q * s2.size()))]; };
    double sum = 0; for (float x : v) sum += x; const double med = pct(0.5);
    int hitch = 0, big = 0; double jit = 0; for (size_t i = 0; i < v.size(); ++i) { if (v[i] > med * 1.5) ++hitch; if (v[i] > med * 2.5) ++big; if (i) jit += std::fabs(v[i] - v[i - 1]); }
    json tail = json::array(); for (size_t i = v.size() > 40 ? v.size() - 40 : 0; i < v.size(); ++i) tail.push_back(std::round(v[i] * 10) / 10);
    return ok({ {"n", v.size()}, {"buffer_full", v.size() >= kFramesCap}, {"avg_ms", sum / v.size()}, {"p50", med}, {"p95", pct(0.95)}, {"p99", pct(0.99)}, {"max", s2.back()},
                {"hitches_1_5x", hitch}, {"hitches_2_5x", big}, {"avg_jump_ms", v.size() > 1 ? jit / (v.size() - 1) : 0.0}, {"last40", tail} });
}
static json perf_cmd(const json& a) {
    {
        std::string msg;
        auto name_ok = [](const std::string& n) { if (n == "all") return true; for (auto k : kJobs) if (n == k) return true; return false; };
        std::lock_guard<std::mutex> lk(g_job_mtx);
        if (a.contains("toggle")) { const std::string n = a["toggle"].get<std::string>(); if (!name_ok(n)) return fail("BAD_JOB", "unknown job " + n);
            if (g_job_off.count(n)) { g_job_off.erase(n); msg = "Resumed " + n; } else { g_job_off.insert(n); msg = "PAUSED " + n; } }
        if (a.contains("off")) { const std::string n = a["off"].get<std::string>(); if (!name_ok(n)) return fail("BAD_JOB", "unknown job " + n); g_job_off.insert(n); msg = "PAUSED " + n; }
        if (a.contains("on")) { const std::string n = a["on"].get<std::string>(); if (n == "all") g_job_off.clear(); else g_job_off.erase(n); msg = n == "all" ? "All trainer jobs running" : "Resumed " + n; }
        if (!msg.empty()) { std::string list; for (auto& x : g_job_off) list += (list.empty() ? "" : ", ") + x; toast(msg + (list.empty() ? "" : "   (paused: " + list + ")")); plog("perf: " + msg); }
    }
    std::lock_guard<std::mutex> lk(g_perf_mtx); json o = json::object();
    for (auto& kv : g_perf) o[kv.first] = { {"avg_ms", kv.second.n ? kv.second.total_ms / kv.second.n : 0.0}, {"max_ms", kv.second.max_ms}, {"calls", kv.second.n}, {"total_ms", kv.second.total_ms} };
    if (a.value("reset", false)) g_perf.clear();
    json off = json::array(); { std::lock_guard<std::mutex> lk2(g_job_mtx); for (auto& x : g_job_off) off.push_back(x); }
    return ok({ {"jobs", o}, {"paused", off} });
}
// Infinite jump (toggle "infinite_jump"): double jump is limited by two counters that go 1 -> 2 in the air
// (Character.JumpCurrentCount, GbxCharacterMovementComponent.JumpedCount). Set back to 1 after a double jump, the game
// always thinks the double jump is still free. Every frame on the game thread (two reads). Tested: 8 presses 1727 cm
// up vs 414 cm normally.
static void ovr_keeper();   // value overrides (defined with the def code)
static void jump_tick() {
    static uintptr_t pawn_c = 0, move_c = 0; static ue::Prop jc, jd;
    bool on = false; { std::lock_guard<std::mutex> lk(g_mtx); on = g_active.contains("infinite_jump"); }
    if (!on) return;
    const uintptr_t pawn = player_pawn(); if (!pawn) { pawn_c = 0; return; }
    if (pawn != pawn_c || !move_c || !ue::alive(move_c)) {
        pawn_c = pawn; jc = ue::find_prop(ue::class_of(pawn), "JumpCurrentCount");
        ue::Prop mp = ue::find_prop(ue::class_of(pawn), "CharacterMovement"); move_c = 0; if (mp) ue::rd_as(pawn + mp.offset, move_c);
        jd = move_c ? ue::find_prop(ue::class_of(move_c), "JumpedCount") : ue::Prop();
    }
    int32_t v = 0;
    if (jc && ue::rd_as(pawn + jc.offset, v) && v >= 2) { const int32_t one = 1; ue::mwrite(pawn + jc.offset, &one, 4); }
    if (move_c && jd && ue::rd_as(move_c + jd.offset, v) && v >= 2) { const int32_t one = 1; ue::mwrite(move_c + jd.offset, &one, 4); }
}
static void pts_tick();
static void keeper_tick() {
    static ULONGLONG last = 0; const ULONGLONG now = GetTickCount64();
    { PerfScope ps("scans"); try { scans_tick(); } catch (...) {} }   // sliced object lists for the jobs below (only while one asked)
    if (job_on("refresh_readouts")) { PerfScope ps("refresh_readouts"); try { refresh_readouts(); } catch (...) {} }
    if (job_on("loot_tick")) { PerfScope ps("loot_tick"); try { loot_tick(); } catch (...) {} }
    if (job_on("pts_tick")) { PerfScope ps("pts_tick"); try { pts_tick(); } catch (...) {} }
    { PerfScope ps("jump_tick"); try { jump_tick(); } catch (...) {} }
    { PerfScope ps("ovr_tick"); try { ovr_keeper(); } catch (...) {} }
    if (job_on("hud_tick")) { PerfScope ps("hud_tick"); try { hud_tick(); } catch (...) {} }
    if (job_on("build_tick")) { PerfScope ps("build_tick"); try { build_tick(); } catch (...) {} }
    if (job_on("diff_tick")) { PerfScope ps("diff_tick"); try { diff_tick(); } catch (...) {} }
    if (job_on("enemy_tick")) { PerfScope ps("enemy_tick"); try { enemy_tick(); } catch (...) {} }
    if (job_on("camera_tick")) { PerfScope ps("camera_tick"); try { camera_tick(); } catch (...) {} }
    else { try { tp_idle(); } catch (...) {} }   // paused: no reticle / hidden L3 left behind
    { static ULONGLONG at = 0; if (job_on("attrs_tick") && now - at > 1000) { at = now; PerfScope ps("attrs_tick"); try { if (session().value("standalone", false)) attrs_tick(); } catch (...) {} } }
    if (g_active.empty() || now - last < 250 || !job_on("toggles")) return; last = now;
    const uintptr_t pawn = player_pawn(); if (!pawn) { g_last_pawn = 0; return; }
    static ULONGLONG solo_at = 0; static bool solo = false;
    if (now - solo_at > 3000) { solo_at = now; solo = session().value("standalone", false); }
    if (!solo) return;
    PerfScope ps_tg("toggles");
    const json& tg = api_tbl("toggles");
    const bool fresh = pawn != g_last_pawn;
    if (fresh) { g_last_pawn = pawn; plog("keeper: new player character - re-applying " + std::to_string(g_active.size()) + " toggles"); }
    static std::map<std::string, ULONGLONG> due;
    // "on" right after a spawn can fail (component not created yet): retried for a few seconds until it works
    static std::set<std::string> on_pending; static ULONGLONG on_until = 0;
    if (fresh) { on_pending.clear(); for (auto& kv : g_active.items()) on_pending.insert(kv.key()); on_until = now + 6000; }
    else if (now > on_until) on_pending.clear();
    for (auto& kv : g_active.items()) {
        if (!tg.contains(kv.key())) continue;
        const json& t = tg[kv.key()];
        const json v = kv.value().is_number() ? kv.value() : t.value("value", json());
        if (on_pending.count(kv.key())) {
            if (!t.contains("on") || run_part(t["on"], v, kv.key() + ".on", !fresh).value("ok", false)) on_pending.erase(kv.key());
        }
        if (t.contains("tick")) {
            ULONGLONG& d = due[kv.key()];
            if (now >= d) { d = now + t.value("every_ms", 1000); PerfScope ps("toggle:" + kv.key()); run_part(t["tick"], v, kv.key(), true); }
        }
    }
}

static int g_enum_names_off = -1;
static json enum_values(const std::string& n) {
    const uintptr_t ec = ue::find_class("Enum");
    for (uintptr_t e : ue::find_all(ec, false, 0)) {
        if (ue::name_of(e) != n) continue;
        auto try_off = [&](int off, json& out) -> bool {
            ue::TArray<uint8_t> arr{}; if (!ue::mread(e + off, &arr, sizeof arr)) return false;
            if (arr.num < 1 || arr.num > 512 || arr.max < arr.num || !arr.data) return false;
            json o = json::object();
            for (int32_t i = 0; i < arr.num; ++i) {
                ue::FName fn{}; int64_t val = 0;
                if (!ue::mread((uintptr_t)arr.data + i * 16, &fn, 8) || !ue::mread((uintptr_t)arr.data + i * 16 + 8, &val, 8)) return false;
                const std::string s = ue::name_str(fn);
                if (s.empty() || s.size() > 120) return false;
                for (char c : s) if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return false;
                o[s] = val;
            }
            out = o; return true;
        };
        json out;
        if (g_enum_names_off >= 0 && try_off(g_enum_names_off, out)) return ok(out);
        for (int off = 0x30; off <= 0x90; off += 8) if (try_off(off, out)) { g_enum_names_off = off; return ok({ {"offset", hexstr(off)}, {"values", out} }); }
        return fail("LAYOUT", "enum names array not found");
    }
    return fail("NOT_FOUND", "enum " + n);
}

// ------------------------------------------------------------------ loot: spawn from item pools, bring pickups to the player
// pool name) - real loot rolls. (ItemPoolFunctionLibrary::SpawnItemsFromItemPoolUsingActor_Drop accepts the handle but
// spawns nothing in this build.) New pickups are thrown a few metres -> they are moved in front of the player.
struct V3 { double x = 0, y = 0, z = 0; };
static bool actor_loc(uintptr_t a, V3& v) {
    json r = ue::call_json(a, "K2_GetActorLocation", json::object(), 0);
    if (!r.value("ok", false)) return false;
    const json& p = r["data"]["ReturnValue"]["value"]; v = { p.value("X", 0.0), p.value("Y", 0.0), p.value("Z", 0.0) }; return true;
}
// Location without a ProcessEvent round trip (tick loops over many actors): Actor.RootComponent -> RelativeLocation, which
// is the world location while the root is not attached to anything. Attached / unreadable -> the engine call.
static bool actor_loc_fast(uintptr_t a, V3& v) {
    static int root_off = -1, loc_off = -1, par_off = -1;   // -2 = layout not found: always the engine call
    if (root_off == -1) {
        const ue::Prop r = ue::find_prop(ue::find_class("Actor"), "RootComponent"), l = ue::find_prop(ue::find_class("SceneComponent"), "RelativeLocation"),
                       pa = ue::find_prop(ue::find_class("SceneComponent"), "AttachParent");
        if (r && l && r.size == 8 && l.size >= 24) { root_off = r.offset; loc_off = l.offset; par_off = pa ? pa.offset : -1; }
        else root_off = -2;
    }
    if (root_off < 0) return actor_loc(a, v);
    uintptr_t root = 0; if (!ue::rd_as(a + root_off, root) || !root) return actor_loc(a, v);
    if (par_off >= 0) { uintptr_t par = 0; if (!ue::rd_as(root + par_off, par) || par) return actor_loc(a, v); }
    double d[3]; if (!ue::mread(root + loc_off, d, sizeof d) || !std::isfinite(d[0]) || !std::isfinite(d[1]) || !std::isfinite(d[2])) return actor_loc(a, v);
    v = { d[0], d[1], d[2] }; return true;
}
static bool actor_fwd(uintptr_t a, V3& v) {
    json r = ue::call_json(a, "GetActorForwardVector", json::object(), 0);
    if (!r.value("ok", false)) return false;
    const json& p = r["data"]["ReturnValue"]["value"]; v = { p.value("X", 0.0), p.value("Y", 0.0), p.value("Z", 0.0) }; return true;
}
static bool actor_move(uintptr_t a, const V3& v) {
    json r = ue::call_json(a, "K2_SetActorLocation", { {"NewLocation", { {"X", v.x}, {"Y", v.y}, {"Z", v.z} }}, {"bSweep", false}, {"bTeleport", true} }, 0);
    // thrown pickups keep their physics velocity after a teleport -> stop them (engine PrimitiveComponent functions)
    if (uintptr_t prim = prop_obj(a, "RootPrimitiveComponent")) {
        ue::call_json(prim, "SetPhysicsLinearVelocity", { {"NewVel", json::object()}, {"bAddToCurrent", false} }, 0);
        ue::call_json(prim, "SetPhysicsAngularVelocityInDegrees", { {"NewAngVel", json::object()}, {"bAddToCurrent", false} }, 0);
    }
    return r.value("ok", false);
}
static std::vector<uintptr_t> all_pickups() { return ue::find_all(ue::find_class("InventoryPickup"), false, 2000); }
// put pickups in a small arc in front of the player (dist cm), lifted a bit so they settle on the ground
static int bring(const std::vector<uintptr_t>& ps, double dist) {
    uintptr_t pawn = player_pawn(); V3 p, f; if (!pawn || !actor_loc(pawn, p) || !actor_fwd(pawn, f)) return 0;
    const double fl = std::sqrt(f.x * f.x + f.y * f.y); if (fl > 0.01) { f.x /= fl; f.y /= fl; }
    int n = 0, i = 0;
    for (uintptr_t a : ps) {
        if (!ue::alive(a)) continue;
        const double side = ((i % 5) - 2) * 45.0, back = (i / 5) * 45.0; ++i;
        V3 t = { p.x + f.x * (dist + back) - f.y * side, p.y + f.y * (dist + back) + f.x * side, p.z + 40 };
        if (actor_move(a, t)) ++n;
    }
    return n;
}
// pool / level / count: when nothing drops (a named legendary has a lowest level, e.g. Cold Shoulder 7), the same roll
// is made again a few levels higher until something drops (raise = false turns that off, bring = false only places nothing).
struct PendingLoot { std::set<uintptr_t> before; std::vector<std::pair<uintptr_t, ULONGLONG>> mine; ULONGLONG until; double dist; V3 at;
                     std::string pool; int level = 0, asked = 0, count = 1, raises = 0; bool bring = true, raise = true, told = false; ULONGLONG retry_at = 0; };
static bool loot_roll(const std::string& pool, int stage, const V3& at, std::string* err);
static std::vector<PendingLoot> g_pending_loot;
static json g_last_loot = json::object();
static void loot_tick() {
    if (g_pending_loot.empty()) return;
    const ULONGLONG now = GetTickCount64();
    g_pick_scan.want();   // sliced pickup list (a full object walk every tick stuttered for the whole 4 s)
    const std::vector<uintptr_t> picks = g_pick_scan.live();
    for (auto& pl : g_pending_loot) {
        std::vector<uintptr_t> fresh;
        for (uintptr_t a : picks) if (!pl.before.count(a)) {
            pl.before.insert(a);
            V3 v; if (actor_loc_fast(a, v) && std::hypot(v.x - pl.at.x, v.y - pl.at.y) > 1500.0) continue;   // not from this spawn (an enemy's drop elsewhere)
            fresh.push_back(a); pl.mine.push_back({ a, now }); }
        if (!fresh.empty() && pl.bring) { const int n = bring(fresh, pl.dist); g_last_loot["brought"] = g_last_loot.value("brought", 0) + n; }
        if (!fresh.empty() && pl.level > pl.asked && !pl.told) { pl.told = true; g_last_loot["level"] = pl.level;
            toast("Dropped at level " + std::to_string(pl.level) + " - the lowest level this item drops at"); plog("loot " + pl.pool + " raised to lvl " + std::to_string(pl.level)); }
        if (pl.mine.empty() && pl.raise && pl.retry_at && now > pl.retry_at && pl.level < 60 && pl.raises < 14) {   // nothing dropped: a few levels higher
            pl.level = std::min(60, pl.level + (pl.level < 20 ? 2 : 5)); ++pl.raises;
            int okn = 0; for (int i = 0; i < pl.count; ++i) if (loot_roll(pl.pool, pl.level, pl.at, nullptr)) ++okn;
            pl.retry_at = okn ? now + 1800 : 0; pl.until = now + 4000;
        }
        // second placement ~0.8 s later: the spawn impulse can still carry them away after the first move
        std::vector<uintptr_t> again;
        for (auto& [a, t] : pl.mine) if (t && now - t > 800) { again.push_back(a); t = 0; }
        if (!again.empty() && pl.bring) bring(again, pl.dist);
    }
    g_pending_loot.erase(std::remove_if(g_pending_loot.begin(), g_pending_loot.end(), [&](const PendingLoot& p) { return now > p.until; }), g_pending_loot.end());
}
// loot catalog: <panel dir>\loot_catalog.json {pools[], lists[], legendaries[{pool,name,maker,type,shiny}]} made by
static json g_catalog;
static const json& catalog() {
    if (g_catalog.is_null()) { const size_t k = g_api_path.find_last_of("\\/"); g_catalog = read_json(g_api_path.substr(0, k) + "\\loot_catalog.json"); if (!g_catalog.is_object()) g_catalog = json::object(); }
    return g_catalog;
}
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static std::string display_to_internal(const std::string& q);   // in-game item name -> Gearbox name (name parts)
static std::string pool_for_item_raw(const std::string& q, bool shiny);
static std::string pool_for_item(const std::string& q, bool shiny) {   // Gearbox name, else the name the game shows ("Eradication" -> Extinction)
    std::string p = pool_for_item_raw(q, shiny); if (!p.empty()) return p;
    const std::string in = display_to_internal(q); return in.empty() ? std::string() : pool_for_item_raw(in, shiny);
}
static std::string pool_for_item_raw(const std::string& q, bool shiny) {
    const std::string lq = lower(q); std::string best; int best_score = -1;
    for (auto& l : catalog().value("legendaries", json::array())) {
        const std::string nm = lower(l.value("name", "")), pool = l.value("pool", "");
        std::string nm2 = nm; nm2.erase(std::remove(nm2.begin(), nm2.end(), ' '), nm2.end());
        std::string lq2 = lq; lq2.erase(std::remove(lq2.begin(), lq2.end(), ' '), lq2.end());
        int sc = -1;
        if (nm2 == lq2) sc = 100; else if (nm2.find(lq2) != std::string::npos) sc = 50; else if (lower(pool).find(lq2) != std::string::npos) sc = 20;
        if (sc < 0) continue;
        if (l.value("shiny", false) == shiny) sc += 5;
        if (sc > best_score) { best_score = sc; best = pool; }
    }
    if (best.empty()) for (auto& p : catalog().value("pools", json::array())) if (lower(p.get<std::string>()).find(lq) != std::string::npos) { best = p.get<std::string>(); break; }
    return best;
}
static json catalog_cmd(const json& a) {
    const std::string f = lower(a.value("filter", std::string())), kind = a.value("kind", std::string("legendaries"));
    const size_t mx = a.value("max", 200);
    json out = json::array();
    for (auto& e : catalog().value(kind, json::array())) {
        const std::string hay = lower(e.is_string() ? e.get<std::string>() : e.dump());
        if (!f.empty() && hay.find(f) == std::string::npos) continue;
        out.push_back(e); if (out.size() >= mx) break;
    }
    return ok({ {"kind", kind}, {"count", out.size()}, {"items", out} });
}
static int player_level();
static bool loot_roll(const std::string& pool, int stage, const V3& at, std::string* err) {   // one SpawnInventoryFromItemPool roll at `at`
    const uintptr_t pawn = player_pawn(); if (!pawn) { if (err) *err = "no player character"; return false; }
    static uintptr_t store = 0; if (!store || !ue::alive(store)) store = ue::cdo_of(ue::find_class("NexusConfigStoreItemPool"));
    if (!store) { if (err) *err = "NexusConfigStoreItemPool missing"; return false; }
    json tr = { {"Rotation", { {"W", 1.0} }}, {"Translation", { {"X", at.x}, {"Y", at.y}, {"Z", at.z} }}, {"Scale3D", { {"X", 1.0}, {"Y", 1.0}, {"Z", 1.0} }} };
    json r = ue::call_json(store, "SpawnInventoryFromItemPool", { {"WorldContextObject", "addr:" + hexstr(pawn)}, {"Transform", tr}, {"GameStage", stage}, {"ItemPoolName", "name:" + pool} }, pawn);
    if (!r.value("ok", false)) { if (err) *err = r.value("message", std::string("failed")); return false; }
    return true;
}
static json loot_spawn(const json& a0) {
    json a = a0;
    if (a.contains("item") && !a.contains("pool")) {
        const std::string p = pool_for_item(a["item"].get<std::string>(), a.value("shiny", false));
        if (p.empty()) return fail("NO_ITEM", "'" + a["item"].get<std::string>() + "' has no loot pool (boss / DLC / reward legendaries) - make it in Gear Maker: Or any legendary");
        a["pool"] = p;
    }
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    uintptr_t pawn = player_pawn(); V3 p, f; if (!pawn || !actor_loc(pawn, p) || !actor_fwd(pawn, f)) return fail("NO_PAWN", "no player character");
    const std::string pool = a.value("pool", std::string());
    if (pool.empty()) return fail("BAD_ARG", "pool (item pool name, e.g. itempool_guns_tor / ItemPoolList_... / itempool_*_legendary_*)");
    const int count = std::clamp(a.value("count", 1), 1, 50);
    int stage = a.value("level", a.value("stage", 0)); if (stage <= 0) stage = player_level();   // 0 / none = the character's own level
    const uintptr_t store = ue::cdo_of(ue::find_class("NexusConfigStoreItemPool"));
    if (!store) return fail("NO_STORE", "NexusConfigStoreItemPool missing");
    PendingLoot pl; for (uintptr_t x : all_pickups()) pl.before.insert(x);
    pl.until = GetTickCount64() + 4000; pl.dist = a.value("dist", 220.0); pl.at = { p.x + f.x * 200, p.y + f.y * 200, p.z + 60 };
    pl.pool = pool; pl.level = pl.asked = stage; pl.count = count; pl.bring = a.value("bring", true); pl.raise = a.value("raise", true);
    int ok_n = 0; std::string last_err;
    for (int i = 0; i < count; ++i) { std::string e; if (loot_roll(pool, stage, pl.at, &e)) ++ok_n; else last_err = e; }
    if (!ok_n) return fail("SPAWN", last_err);
    pl.retry_at = GetTickCount64() + 1800;
    g_pending_loot.push_back(pl);
    g_last_loot = { {"pool", pool}, {"calls", ok_n}, {"level", stage}, {"brought", 0} };
    plog("loot " + pool + " x" + std::to_string(ok_n) + " lvl " + std::to_string(stage));
    return ok(g_last_loot);
}
static json pickups_cmd(const json& a) {
    uintptr_t pawn = player_pawn(); V3 p; if (pawn) actor_loc(pawn, p);
    const double r = a.value("radius_m", 1e9) * 100.0;
    json out = json::array();
    for (uintptr_t x : all_pickups()) {
        V3 v; if (!actor_loc(x, v)) continue;
        const double d = std::sqrt((v.x - p.x) * (v.x - p.x) + (v.y - p.y) * (v.y - p.y) + (v.z - p.z) * (v.z - p.z));
        if (d > r) continue;
        double iid = 0; { uintptr_t b = 0; ue::Prop leaf; if (ue::resolve(x, ue::class_of(x), "Item.data.InstanceId", b, leaf)) iid = ue::get_num(b, leaf); }
        out.push_back({ {"addr", hexstr(x)}, {"id", iid}, {"dist_m", std::round(d) / 100.0} });
    }
    return ok(out);
}
static json vacuum(const json& a) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    uintptr_t pawn = player_pawn(); V3 p; if (!pawn || !actor_loc(pawn, p)) return fail("NO_PAWN", "no player character");
    const double r = a.value("radius_m", 60.0) * 100.0;
    std::vector<uintptr_t> sel;
    for (uintptr_t x : all_pickups()) { V3 v; if (actor_loc(x, v) && std::hypot(v.x - p.x, v.y - p.y) <= r) sel.push_back(x); }
    return ok({ {"moved", bring(sel, a.value("dist", 220.0))}, {"found", (int)sel.size()} });
}

// ------------------------------------------------------------------ stat editor: Gearbox attributes (game's own modifiers)
// multiplier, ground_speed_scale, weapon_fire_rate, weapon_reload_time, weapon_spread, shield_capacity ...
// Read: GbxAttributeBlueprintLibrary::GetValueOfAttribute(Name, actor, default).
// Write: AddModifierToGbxAttribute(def ("<name>","GbxAttributeDef"), actor, type, value, bAutoRefresh) - the same
// modifier system buffs use; type 0 ScaleAdd, 1 PreAdd, 2 PostAdd, 3 ScaleMultiply, 4 OverrideBaseValue.
// Removed with RemoveAttributeModifier(handle). Active modifiers are saved (<data>\attrs.json) and re-applied on a new
// player character (map change / respawn: the old handles die with the old pawn).
struct AttrMod { std::string name; int type = 3; double value = 1; std::vector<uint8_t> handle; uintptr_t pawn = 0; };
static std::map<std::string, AttrMod> g_mods;
static std::string attrs_file() { return g_data_dir + "\\attrs.json"; }
static uintptr_t attr_lib() { static uintptr_t l = 0; if (!l || !ue::alive(l)) l = ue::cdo_of(ue::find_class("GbxAttributeBlueprintLibrary")); return l; }
static void save_attrs() {
    json j = json::object();
    for (auto& [k, m] : g_mods) j[k] = { {"type", m.type}, {"value", m.value}, {"handle", to_hex(m.handle.data(), m.handle.size())}, {"pawn", hexstr(m.pawn)} };
    save_toggles(); std::ofstream(attrs_file()) << j.dump(1);
}
static json attr_get(const std::string& n, uintptr_t on = 0) {
    if (!on) on = player_pawn(); if (!on) return json{ {"error", "no player character"} };
    json r = ue::call_json(attr_lib(), "GetValueOfAttribute", { {"AttributeDef", "name:" + n}, {"ContextSource", "addr:" + hexstr(on)}, {"DefaultValue", -98765.0} }, 0);
    if (!r.value("ok", false)) return json{ {"error", r.value("message", std::string("failed"))} };
    const double v = r["data"]["ReturnValue"].value("value", -98765.0);
    if (std::fabs(v + 98765.0) < 0.01) return json{ {"error", "attribute not on this actor: " + n} };
    return v;
}
static bool attr_remove(AttrMod& m) {
    if (m.handle.empty()) return true;
    bool okv = false;
    if (m.pawn && ue::alive(m.pawn) && m.pawn == player_pawn()) {
        ue::Params p = ue::params_for(attr_lib(), "RemoveAttributeModifier");
        ue::Prop h = p ? p.param("ModifierHandle") : ue::Prop();
        if (h) { memcpy(p.at(h), m.handle.data(), std::min<size_t>(h.size, m.handle.size())); std::string e; okv = ue::call(attr_lib(), p, &e) && p.ret<bool>(); }
    }
    m.handle.clear(); return okv;
}
static bool attr_apply(AttrMod& m, std::string* why) {
    const uintptr_t pawn = player_pawn(); if (!pawn) { if (why) *why = "no player character"; return false; }
    ue::Params p = ue::params_for(attr_lib(), "AddModifierToGbxAttribute"); if (!p) { if (why) *why = "AddModifierToGbxAttribute missing"; return false; }
    json r = ue::call_json(attr_lib(), "AddModifierToGbxAttribute", { {"attribute", "def:(\"" + m.name + "\",\"GbxAttributeDef\")"},
        {"ContextSource", "addr:" + hexstr(pawn)}, {"ModifierType", m.type}, {"modifiervalue", m.value}, {"bAutoRefresh", true} }, 0);
    if (!r.value("ok", false)) { if (why) *why = r.value("message", std::string("failed")); return false; }
    const std::string hx = r["data"]["ReturnValue"].value("bytes", std::string());
    m.handle.clear(); for (size_t i = 0; i + 1 < hx.size(); i += 3) m.handle.push_back((uint8_t)std::stoi(hx.substr(i, 2), nullptr, 16));
    m.pawn = pawn; return true;
}
static json attr_set(const json& a) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    const std::string n = a.value("name", std::string()); if (n.empty()) return fail("BAD_ARG", "name");
    json before = attr_get(n); if (before.is_object()) return fail("NO_ATTR", before.value("error", std::string()));
    if (a.contains("reset") || (a.contains("v") && a["v"].is_null())) {
        auto it = g_mods.find(n); if (it != g_mods.end()) { attr_remove(it->second); g_mods.erase(it); save_attrs(); }
        return ok({ {"name", n}, {"value", attr_get(n)}, {"reset", true} });
    }
    if (!a.contains("v") || !a["v"].is_number()) return fail("NEEDS_VALUE", "v = number");
    AttrMod m; m.name = n; m.value = a["v"].get<double>();
    std::string t = a.value("mode", std::string("set"));
    if (t == "speed") {
        if (!(m.value > 0.01)) return fail("BAD_ARG", "v > 0");
        if (std::fabs(m.value - 1.0) < 1e-6) {
            auto it = g_mods.find(n); if (it != g_mods.end()) { attr_remove(it->second); g_mods.erase(it); save_attrs(); }
            return ok({ {"name", n}, {"value", attr_get(n)}, {"reset", true} });
        }
        m.value = 1.0 / m.value; t = "mult";
    }
    m.type = t == "mult" ? 3 : t == "add" ? 2 : t == "scale_add" ? 0 : 4;   // default "set" = OverrideBaseValue
    auto it = g_mods.find(n); if (it != g_mods.end()) { attr_remove(it->second); g_mods.erase(it); }
    std::string why; if (!attr_apply(m, &why)) return fail("ATTR", why);
    g_mods[n] = m; save_attrs();
    json after = attr_get(n);
    plog("attr " + n + " " + t + " " + std::to_string(m.value) + " -> " + after.dump());
    return ok({ {"name", n}, {"before", before}, {"after", after}, {"mode", t} });
}
static json attr_clear(const json& a) {
    int n = 0; for (auto& [k, m] : g_mods) { attr_remove(m); ++n; }
    g_mods.clear(); save_attrs(); return ok({ {"cleared", n} });
}
static void attrs_tick() {   // re-apply on a new pawn (zone change / respawn)
    if (g_mods.empty()) return;
    const uintptr_t pawn = player_pawn(); if (!pawn) return;
    bool changed = false;
    for (auto& [k, m] : g_mods) if (m.pawn != pawn) { changed = true; std::string w; if (attr_apply(m, &w)) plog("attr re-applied: " + k); else { m.pawn = pawn; plog("attr " + k + ": " + w); } }
    if (changed) save_attrs();
}

// ------------------------------------------------------------------ enemies: kill / list (game's own OakCharacter::Die)
static std::string team_of(uintptr_t a) {
    static uintptr_t lib = 0; if (!lib) lib = ue::cdo_of(ue::find_class("GbxTeamFunctionLibrary"));
    json r = ue::call_json(lib, "GetTeam", { {"AIActor", "addr:" + hexstr(a)} }, 0);
    return r.value("ok", false) ? r["data"]["ReturnValue"].value("value", std::string()) : std::string();
}
// team per character, remembered for 3 s (tick loops asked the game for every character every pass). set_team() drops the entry.
static std::unordered_map<uintptr_t, std::pair<std::string, ULONGLONG>> g_team_cache;
static std::string team_cached(uintptr_t a) {
    const ULONGLONG now = GetTickCount64(); auto it = g_team_cache.find(a);
    if (it != g_team_cache.end() && now - it->second.second < 3000) return it->second.first;
    if (g_team_cache.size() > 4000) g_team_cache.clear();
    std::string t = team_of(a); g_team_cache[a] = { t, now }; return t;
}
static bool friendly_team(const std::string& t) {
    return t.empty() || t == "Team_Player" || t == "Team_NPC" || t.find("Friendly") != std::string::npos || t.find("Ally") != std::string::npos;
}
static json enemies_cmd(const json& a, bool kill);
// freeze = Actor.CustomTimeDilation (GbxAttributeFloat) 0 on hostiles (memory write: no game function sets it); 1 = normal
static json freeze_cmd(const json& a) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    json list = enemies_cmd(a, false); if (!list.value("ok", false)) return list;
    const double td = a.value("on", true) ? a.value("speed", 0.0) : 1.0;
    uintptr_t pawn = player_pawn(); V3 p; actor_loc(pawn, p);
    const double r = a.value("radius_m", 80.0) * 100.0; int n = 0;
    for (uintptr_t c : ue::find_all(ue::find_class("OakCharacter"), false, 3000)) {
        if (c == pawn) continue; V3 v; if (!actor_loc(c, v)) continue;
        if (std::sqrt((v.x - p.x) * (v.x - p.x) + (v.y - p.y) * (v.y - p.y) + (v.z - p.z) * (v.z - p.z)) > r) continue;
        if (friendly_team(team_of(c))) continue;
        std::string w; if (write_field(c, "CustomTimeDilation", td, &w)) ++n;
    }
    plog("freeze " + std::to_string(n) + " -> time " + std::to_string(td));
    return ok({ {"changed", n}, {"time_dilation", td} });
}
static json enemies_cmd(const json& a, bool kill) {
    if (kill) { json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only"); }
    uintptr_t pawn = player_pawn(); V3 p; if (!pawn || !actor_loc(pawn, p)) return fail("NO_PAWN", "no player character");
    const double r = a.value("radius_m", 80.0) * 100.0;
    const int type = a.value("type", 0);
    json out = json::array(); int killed = 0;
    for (uintptr_t c : ue::find_all(ue::find_class("OakCharacter"), false, 3000)) {
        if (c == pawn) continue;
        V3 v; if (!actor_loc(c, v)) continue;
        const double d = std::sqrt((v.x - p.x) * (v.x - p.x) + (v.y - p.y) * (v.y - p.y) + (v.z - p.z) * (v.z - p.z));
        if (d > r) continue;
        const std::string t = team_of(c);
        if (friendly_team(t)) continue;
        { json h = ue::call_json(c, "IsHealthy", json::object(), 0);   // dead / dying characters are skipped
          if (h.value("ok", false) && h["data"]["ReturnValue"].value("value", 1.0) == 0) continue; }
        json e = { {"name", ue::name_of(c)}, {"addr", hexstr(c)}, {"team", t}, {"dist_m", std::round(d) / 100.0} };
        if (kill) {
            json k = ue::call_json(c, "Die", { {"type", type} }, 0); e["killed"] = k.value("ok", false);
            if (k.value("ok", false)) {
                ++killed;
                // XP for the kill like a normal kill (game's OakExperienceStatics::AwardKillExperienceToParty; XP multiplier applies)
                if (a.value("xp", true)) {
                    static uintptr_t xl = 0; if (!xl || !ue::alive(xl)) xl = ue::cdo_of(ue::find_class("OakExperienceStatics"));
                    ue::call_json(xl, "AwardKillExperienceToParty", { {"ContextActor", "addr:" + hexstr(c)}, {"OptionalKiller", "addr:" + hexstr(player_pc())} }, 0);
                }
            }
        }
        out.push_back(e);
    }
    if (kill) plog("killed " + std::to_string(killed) + " within " + std::to_string((int)(r / 100)) + " m");
    return ok({ {"enemies", out}, {"killed", killed} });
}

// ------------------------------------------------------------------ fast travel (game's OakPlayerController::RequestTeleportToStation)
// Stations = GameDataHandle category 0x6000 named "World_P.<station>" (FTS_ fast travel, RS_/RTS_ respawn points ...).
static json g_stations;
static const json& stations() {
    if (g_stations.is_null()) { const size_t k = g_api_path.find_last_of("\\/"); g_stations = read_json(g_api_path.substr(0, k) + "\\stations.json"); if (!g_stations.is_array()) g_stations = json::array(); }
    return g_stations;
}
static json stations_cmd(const json& a) {
    const std::string f = lower(a.value("filter", std::string())); json out = json::array();
    for (auto& s : stations()) { if (!f.empty() && lower(s.dump()).find(f) == std::string::npos) continue; out.push_back(s); if (out.size() >= (size_t)a.value("max", 300)) break; }
    return ok({ {"count", out.size()}, {"stations", out} });
}
static json travel_cmd(const json& a) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    std::string q = a.value("name", std::string()); if (q.empty()) return fail("BAD_ARG", "name (BL.STATIONS lists them)");
    std::string full;
    if (q.find('.') != std::string::npos) full = q;
    else {   // best match: exact short name, then fast-travel stations containing the text, then anything containing it
        const std::string lq = lower(q); int best = -1;
        for (auto& st : stations()) {
            const std::string sh = lower(st.value("short", "")); int sc = -1;
            if (sh == lq) sc = 100; else if (sh.find(lq) != std::string::npos) sc = st.value("kind", "") == "FTS" ? 60 : 40;
            if (sc > best) { best = sc; full = st.value("name", ""); }
        }
        if (full.empty()) return fail("NO_STATION", "no station matches '" + q + "'");
    }
    uintptr_t pc = player_pc(); if (!pc) return fail("NO_PC", "no player controller");
    json r = ue::call_json(pc, "RequestTeleportToStation", { {"station", "gdh:" + full + ":24576"}, {"bForRespawn", false}, {"bLocalOnly", false} }, 0);
    plog("travel " + full + " -> " + (r.value("ok", false) ? "ok" : r.value("message", std::string("failed"))));
    if (r.value("ok", false)) r["data"]["station"] = full;
    return r;
}

// ------------------------------------------------------------------ spawn characters (game's SpawnBlueprintLibrary::GbxSpawnActorAtTransform)
// GbxActorDef = GameDataHandle category 0x4000 named like the character ("Char_PsychoBasic_Female"). The function is
// latent: it needs a LatentActionInfo with a CallbackTarget (the pawn) or it does nothing. List: panel\actors.json.
static json g_actors;
static const json& actors() {
    if (g_actors.is_null()) { const size_t k = g_api_path.find_last_of("\\/"); g_actors = read_json(g_api_path.substr(0, k) + "\\actors.json"); if (!g_actors.is_array()) g_actors = json::array(); }
    return g_actors;
}
static std::string actor_for(const std::string& q) {
    if (q.rfind("Char_", 0) == 0 || q.rfind("char_", 0) == 0) return q;
    const std::string lq = lower(q); std::string best; size_t bl = 9999;
    for (auto& a : actors()) { const std::string n = a.is_string() ? a.get<std::string>() : a.value("name", ""); if (lower(n).find(lq) != std::string::npos && n.size() < bl) { best = n; bl = n.size(); } }
    return best;
}
static json spawn_cmd(const json& a) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    const std::string def = actor_for(a.value("name", std::string()));
    if (def.empty()) return fail("NO_ACTOR", "no character matches (BL.ACTORS {filter})");
    uintptr_t pawn = player_pawn(); V3 p, f; if (!pawn || !actor_loc(pawn, p) || !actor_fwd(pawn, f)) return fail("NO_PAWN", "no player character");
    const int count = std::clamp(a.value("count", 1), 1, 20); const double dist = a.value("dist", 800.0);
    const uintptr_t lib = ue::cdo_of(ue::find_class("SpawnBlueprintLibrary"));
    static int uuid = 9000; int n = 0; std::string err;
    for (int i = 0; i < count; ++i) {
        const double side = (i - (count - 1) / 2.0) * 250.0;
        json tr = { {"Rotation", { {"W", 1.0} }}, {"Translation", { {"X", p.x + f.x * dist - f.y * side}, {"Y", p.y + f.y * dist + f.x * side}, {"Z", p.z + 50} }}, {"Scale3D", { {"X", 1.0}, {"Y", 1.0}, {"Z", 1.0} }} };
        json r = ue::call_json(lib, "GbxSpawnActorAtTransform", { {"Context", "addr:" + hexstr(pawn)}, {"GbxActorDef", "gdh:" + def + ":16384"}, {"Transform", tr},
            {"LatentInfo", { {"Linkage", -1}, {"UUID", ++uuid}, {"CallbackTarget", "addr:" + hexstr(pawn)} }} }, pawn);
        if (r.value("ok", false)) ++n; else err = r.value("message", std::string("failed"));
    }
    plog("spawn " + def + " x" + std::to_string(n));
    if (!n) return fail("SPAWN", err);
    return ok({ {"actor", def}, {"calls", n} });
}

// ------------------------------------------------------------------ DIFFICULTY TWEAKS (BL.DIFF) - all through the game's own systems
// Per enemy (each hostile OakCharacter within 250 m gets its own attribute modifiers, removed again when settings change):
//   health   ai_health_resource_01..03_max_value  ScaleMultiply (every bar: flesh / shield / armor), then
//            DamageStatics::RefillHealthPercent per HealthType_AI_* so a fresh enemy starts full
//   damage   damage_dealt_multiplier   speed ground_speed_scale   accuracy weapon_ai_accuracy   fire_rate weapon_fire_rate
//   reflex   Actor.CustomTimeDilation (the enemy's whole clock: animations, attacks, reactions)
// Numbers: every new hostile has a chance to bring "extra" copies (SpawnBlueprintLibrary::GbxSpawnActorAtTransform with
//   its own actor def); "badass" % of copies use the Badass variant when one exists. Bosses are never copied.
// Loot: on death, "loot" % chance of an extra gun (itempool_guns_all), "legendary" % chance of a legendary (x5 for bosses).
// Bosses: "boss_minutes" > 0 = a random replay/rift boss shows up near you every N minutes while you are out among enemies.
// Player side: XP / cash multipliers = the game's Att_PlayerDifficulty_* attributes (same modifier store as the stat editor).
struct DiffPreset { const char* name; const char* tip; double hp, dmg, spd, rfx, acc, fire, extra, badass, loot, leg, xp, cash, boss; };
static const DiffPreset kDiffPresets[] = {
    { "Normal (trainer off)",          "The game's own difficulty, untouched.",                          1,    1,    1,    1,    1,    1,    0,   0,  0,  0,  1,    1,   0 },
    { "Baby's First Murder Spree",     "Enemies practically shoot themselves for you.",                  0.25, 0.1,  0.7,  0.8,  0.5,  0.5,  0,   0,  0,  0,  1,    1,   0 },
    { "Claptrap Could Beat This",      "That's not a compliment, dumbass.",                              0.35, 0.2,  0.75, 0.85, 0.6,  0.6,  0,   0,  0,  0,  1,    1,   0 },
    { "My Grandma Hits Harder",        "She's 97. And she's disappointed in you.",                       0.5,  0.35, 0.8,  0.9,  0.7,  0.7,  0,   0,  0,  0,  1,    1,   0 },
    { "Enemies Forgot Their Guns",     "They showed up for emotional support.",                          0.6,  0.15, 0.9,  0.95, 0.3,  0.4,  0,   0,  0,  0,  1,    1,   0 },
    { "Participation Trophy Hunter",   "Everybody's a winner! Except the enemies.",                      0.75, 0.6,  1,    1,    0.85, 0.85, 0,   0,  25, 2,  1,    1,   0 },
    { "Mildly Spicy Meatballs",        "You might actually need to reload.",                             1,    1,    1,    1,    1,    1,    0,   0,  10, 1,  1,    1,   0 },
    { "Shit Just Got Real-ish",        "Bullets are starting to hurt. Weird, right?",                    1.5,  1.3,  1.05, 1,    1.15, 1.1,  0.25,0,  10, 1,  1.2,  1.2, 0 },
    { "Psychos on Bath Salts",         "They have absolutely nothing to lose.",                          1.75, 1.5,  1.35, 1.2,  1.2,  1.2,  0.5, 5,  15, 1,  1.3,  1.3, 0 },
    { "Badassery Has Consequences",    "Your health insurance has been canceled.",                       2.5,  1.8,  1.15, 1.1,  1.3,  1.25, 0.5, 15, 20, 2,  1.5,  1.5, 0 },
    { "Your Shield Is a Suggestion",   "It's mostly there for decoration.",                              3,    2.5,  1.2,  1.1,  1.4,  1.3,  1,   20, 25, 3,  1.75, 1.75,0 },
    { "Respawn Simulator 3000",        "At least the loading screen looks nice.",                        4,    3.5,  1.25, 1.15, 1.5,  1.4,  1.5, 25, 30, 4,  2,    2,   15 },
    { "God Has Left Pandora",          "You're on your own, you magnificent idiot.",                     6,    5,    1.3,  1.2,  1.6,  1.5,  2,   35, 40, 6,  2.5,  2,   10 },
    { "WTF IS THIS BULLSHIT?!",        "Did that bastard just one-shot me?! YES. YES, HE DID.",          10,   10,   1.4,  1.3,  2,    1.75, 3,   50, 50, 10, 3,    3,   5 },
};
static const char* kChaosNames[] = {
    "Enemies Have Plot Armor", "I'm Just Here for the Loot, Asshole", "Who Gave the Skags Steroids?!", "Bullets Now Contain Bullets",
    "Congratulations, You're the Loot", "The Developers Hate You", "Even Your Respawn Point Is Scared", "Health Bar Sold Separately",
    "Your Last Words Were \"EZ\"", "New-U Station Loyalty Program", "You're Not the Main Character Anymore", "OH FUCK, OH FUCK, OH FUCK!",
    "Loot Goblin's Revenge", "Bullet Sponge Appreciation Day", "The Bandits Unionized", "Ammo Is a Lifestyle Choice",
    "Somebody Fed the Bosses After Midnight", "Your Insurance Called. They're Laughing.", "Badass Is the New Normal", "Pain, Now in 4K",
};
static const json g_diff_def = { {"on", false}, {"preset", 0}, {"title", ""}, {"health", 1.0}, {"damage", 1.0}, {"speed", 1.0}, {"reflex", 1.0},
    {"accuracy", 1.0}, {"fire_rate", 1.0}, {"arm", 0.0}, {"budget", 1.0}, {"extra", 0.0}, {"badass", 0.0}, {"loot", 0.0}, {"legendary", 0.0}, {"xp", 1.0}, {"cash", 1.0}, {"boss_minutes", 0.0} };
static json g_diff = g_diff_def;
static json g_diff_snap = g_diff_def;   // copy the panel thread reads (under g_mtx)
static int g_diff_gen = 0;
struct DiffEnemy { std::vector<std::vector<uint8_t>> h; std::string name; bool ignore = false, clone = false, dead = false, boss = false, armed = false; int gen = -1; double td = 1; };
static bool enemy_arm(uintptr_t c, int slot, std::string* label);
static std::map<uintptr_t, DiffEnemy> g_dene;
static std::map<std::string, int> g_clone_expect;
static std::map<std::string, int> g_swap_expect;          // spawn swap: characters we asked the game for (never replaced again)
static std::map<std::string, ULONGLONG> g_expect_at;      // when a def was last expected: a spawn that never shows up expires
static bool g_dene_dirty = false;
static void expect_spawn(const std::string& def, bool swap) { ++g_clone_expect[def]; if (swap) ++g_swap_expect[def]; g_expect_at[def] = GetTickCount64(); }
static void expect_expire() {   // 15 s without the character appearing: forget it (else the next natural enemy of that def counts as ours)
    const ULONGLONG now = GetTickCount64();
    for (auto it = g_expect_at.begin(); it != g_expect_at.end();) {
        if (now - it->second > 15000) { g_clone_expect.erase(it->first); g_swap_expect.erase(it->first); it = g_expect_at.erase(it); } else ++it;
    }
}
static ULONGLONG g_boss_next = 0, g_clone_window = 0, g_last_hostile = 0; static int g_clone_count = 0;
static json g_diff_stats = { {"copies", 0}, {"badass", 0}, {"drops", 0}, {"legendary", 0}, {"bosses", 0}, {"modded", 0} };
static std::string diff_file() { return g_data_dir + "\\difficulty.json"; }
static uint64_t g_rng = 0x9E3779B97F4A7C15ull ^ GetTickCount64();
static double rnd01() { g_rng ^= g_rng << 13; g_rng ^= g_rng >> 7; g_rng ^= g_rng << 17; return (double)(g_rng >> 11) * (1.0 / 9007199254740992.0); }

static const std::set<std::string>& actor_set() {
    static std::set<std::string> s;
    if (s.empty()) for (auto& a : actors()) s.insert(a.is_string() ? a.get<std::string>() : a.value("name", ""));
    return s;
}
static std::string base_name(std::string n) {   // "Char_CatAdult_2147473392" -> "Char_CatAdult"
    const size_t k = n.find_last_of('_');
    if (k != std::string::npos && n.size() - k > 6 && n.find_first_not_of("0123456789", k + 1) == std::string::npos) n.resize(k);
    return n;
}
static std::vector<uint8_t> hex_bytes(const std::string& hx) {
    std::vector<uint8_t> b; for (size_t i = 0; i + 1 < hx.size(); i += 3) b.push_back((uint8_t)std::stoul(hx.substr(i, 2), nullptr, 16)); return b;
}
static bool add_mod(uintptr_t on, const std::string& attr, int type, double v, std::vector<uint8_t>* h) {
    json r = ue::call_json(attr_lib(), "AddModifierToGbxAttribute", { {"attribute", "def:(\"" + attr + "\",\"GbxAttributeDef\")"},
        {"ContextSource", "addr:" + hexstr(on)}, {"ModifierType", type}, {"modifiervalue", v}, {"bAutoRefresh", true} }, 0);
    if (!r.value("ok", false)) return false;
    if (h) *h = hex_bytes(r["data"]["ReturnValue"].value("bytes", std::string()));
    return true;
}
static void remove_mod(const std::vector<uint8_t>& handle) {
    ue::Params p = ue::params_for(attr_lib(), "RemoveAttributeModifier");
    ue::Prop h = p ? p.param("ModifierHandle") : ue::Prop();
    if (h) { memcpy(p.at(h), handle.data(), std::min<size_t>(h.size, handle.size())); std::string e; ue::call(attr_lib(), p, &e); }
}
static bool spawn_char_at(const std::string& def, const V3& at) {
    const uintptr_t pawn = player_pawn(); if (!pawn) return false;
    static uintptr_t lib = 0; if (!lib || !ue::alive(lib)) lib = ue::cdo_of(ue::find_class("SpawnBlueprintLibrary"));
    static int uuid = 19000;
    json tr = { {"Rotation", { {"W", 1.0} }}, {"Translation", { {"X", at.x}, {"Y", at.y}, {"Z", at.z} }}, {"Scale3D", { {"X", 1.0}, {"Y", 1.0}, {"Z", 1.0} }} };
    json r = ue::call_json(lib, "GbxSpawnActorAtTransform", { {"Context", "addr:" + hexstr(pawn)}, {"GbxActorDef", "gdh:" + def + ":16384"}, {"Transform", tr},
        {"LatentInfo", { {"Linkage", -1}, {"UUID", ++uuid}, {"CallbackTarget", "addr:" + hexstr(pawn)} }} }, pawn);
    return r.value("ok", false);
}
static int player_level() {
    const json& ros = api_tbl("readouts"); if (!ros.contains("level")) return 50;
    json v = readout(ros["level"]); return v.is_number() ? std::clamp((int)v.get<double>(), 1, 100) : 50;
}
static bool loot_at(const std::string& pool, const V3& at) {
    const uintptr_t pawn = player_pawn(); if (!pawn) return false;
    static uintptr_t store = 0; if (!store || !ue::alive(store)) store = ue::cdo_of(ue::find_class("NexusConfigStoreItemPool"));
    json tr = { {"Rotation", { {"W", 1.0} }}, {"Translation", { {"X", at.x}, {"Y", at.y}, {"Z", at.z + 80} }}, {"Scale3D", { {"X", 1.0}, {"Y", 1.0}, {"Z", 1.0} }} };
    json r = ue::call_json(store, "SpawnInventoryFromItemPool", { {"WorldContextObject", "addr:" + hexstr(pawn)}, {"Transform", tr}, {"GameStage", player_level()}, {"ItemPoolName", "name:" + pool} }, pawn);
    return r.value("ok", false);
}

static double dnum(const char* k) { return g_diff.value("on", false) ? g_diff.value(k, 1.0) : 1.0; }
static void diff_mod(uintptr_t c, DiffEnemy& e) {
    for (auto& h : e.h) remove_mod(h);
    e.h.clear();
    json pct = attr_get("actor_current_total_health_pct", c);
    const bool full = pct.is_number() && pct.get<double>() > 0.98;
    auto add = [&](const std::string& n, double v, bool check) {
        if (std::fabs(v - 1) < 1e-3) return;
        if (check && attr_get(n, c).is_object()) return;   // this enemy has no such stat (no 2nd health bar, no gun ...)
        std::vector<uint8_t> h; if (add_mod(c, n, 3, v, &h) && !h.empty()) e.h.push_back(h);
    };
    const double hp = dnum("health");
    for (int i = 1; i <= 3; ++i) add("ai_health_resource_0" + std::to_string(i) + "_max_value", hp, i > 1);
    add("damage_dealt_multiplier", dnum("damage"), false);
    add("ground_speed_scale", dnum("speed"), false);
    add("weapon_ai_accuracy", dnum("accuracy"), true);
    add("weapon_fire_rate", dnum("fire_rate"), true);
    if (full) {
        static uintptr_t ds = 0; if (!ds || !ue::alive(ds)) ds = ue::cdo_of(ue::find_class("DamageStatics"));
        static const char* kHT[] = { "HealthType_AI_Flesh", "HealthType_AI_Armor", "HealthType_AI_Energy", "HealthType_AI_Bone",
                                     "HealthType_AI_Corruption", "HealthType_AI_Eridium", "HealthType_AI_Ordonite" };
        for (const char* t : kHT) ue::call_json(ds, "RefillHealthPercent", { {"Context", "addr:" + hexstr(c)}, {"HealthType", std::string("def:(\"") + t + "\",\"HealthTypeDef\")"}, {"Percent", 1.0}, {"MaxPercent", 1.0} }, 0);
    }
    if (!e.armed && g_diff.value("on", false) && rnd01() * 100 < g_diff.value("arm", 0.0) && !attr_get("weapon_fire_rate", c).is_object()) {   // gun enemies only
        e.armed = true; std::string l; if (enemy_arm(c, -1, &l)) g_diff_stats["armed"] = g_diff_stats.value("armed", 0) + 1;
    }
    const double td = dnum("reflex");
    if (std::fabs(td - e.td) > 1e-3) { std::string w; if (write_field(c, "CustomTimeDilation", td, &w)) e.td = td; }
    if (e.gen < 0 && g_diff.value("on", false)) g_diff_stats["modded"] = g_diff_stats.value("modded", 0) + 1;
    e.gen = g_diff_gen; g_dene_dirty = true;
}
// REMOVES the old modifiers before adding its own (they used to stack: health x health) and does not copy enemies twice.
static std::string diff_live_file() { return g_data_dir + "\\diff_live.json"; }
static void diff_live_save() {
    json en = json::object();
    for (auto& [c, e] : g_dene) {
        json hs = json::array(); for (auto& h : e.h) if (!h.empty()) hs.push_back(to_hex(h.data(), h.size()));
        en[hexstr(c)] = { {"h", hs}, {"name", e.name}, {"ignore", e.ignore}, {"clone", e.clone}, {"dead", e.dead}, {"boss", e.boss}, {"armed", e.armed}, {"td", e.td} };
    }
    std::ofstream(diff_live_file()) << json({ {"pid", (uint64_t)GetCurrentProcessId()}, {"enemies", en} }).dump();
}
static void diff_live_load() {   // init: same game process only (addresses and handles mean nothing in another run)
    json j = read_json(diff_live_file());
    if (!j.is_object() || !j.contains("pid") || !j["pid"].is_number() || j["pid"].get<uint64_t>() != (uint64_t)GetCurrentProcessId()) return;
    const json en = j.value("enemies", json::object()); if (!en.is_object()) return;
    for (auto& kv : en.items()) {
        const json& x = kv.value(); if (!x.is_object()) continue;
        DiffEnemy e; const json hs = x.value("h", json::array());
        if (hs.is_array()) for (auto& h : hs) if (h.is_string()) e.h.push_back(hex_bytes(h.get<std::string>()));
        e.name = x.value("name", std::string()); e.ignore = x.value("ignore", false); e.clone = x.value("clone", false); e.dead = x.value("dead", false);
        e.boss = x.value("boss", false); e.armed = x.value("armed", false); e.td = x.value("td", 1.0); e.gen = -1;   // gen -1: re-done once (old handles removed first)
        const uintptr_t c = std::stoull(kv.key(), nullptr, 16); if (c) g_dene[c] = e;
    }
}
static void diff_clone(const std::string& name, const V3& v, const V3& pl) {
    const double extra = g_diff.value("extra", 0.0); if (extra <= 0) return;
    if (name == "Char_CatElder" || name == "Char_CatAdult" || name == "Char_CatYoung") return;   // the HQ pets
    const auto& as = actor_set(); if (!as.count(name)) return;
    const ULONGLONG now = GetTickCount64();
    if (now - g_clone_window > 60000) { g_clone_window = now; g_clone_count = 0; }
    int n = (int)extra; if (rnd01() < extra - n) ++n;
    for (int i = 0; i < n && g_clone_count < 40; ++i) {   // max 40 extra enemies a minute
        std::string def = name;
        if (rnd01() * 100 < g_diff.value("badass", 0.0)) {
            for (std::string b : { name, name.substr(0, name.find_last_of('_')) }) {
                const size_t k = b.find("Basic"); if (k == std::string::npos) continue;
                b.replace(k, 5, "Badass"); if (as.count(b)) { def = b; break; }
            }
        }
        // on the far side of the original, away from the player: they look like they were always there
        const double ang = std::atan2(v.y - pl.y, v.x - pl.x) + (rnd01() - 0.5) * 1.6, r = 250 + rnd01() * 300;
        const V3 at{ v.x + std::cos(ang) * r, v.y + std::sin(ang) * r, v.z + 60 };
        if (spawn_char_at(def, at)) {
            ++g_clone_count; expect_spawn(def, false);
            g_diff_stats["copies"] = g_diff_stats.value("copies", 0) + 1;
            if (def != name) g_diff_stats["badass"] = g_diff_stats.value("badass", 0) + 1;
        }
    }
}
static void diff_drop(const V3& at, const DiffEnemy& e) {
    static const char* kLeg[] = { "itempool_ar_05_legendary", "itempool_ps_05_legendary", "itempool_sg_05_legendary", "itempool_sm_05_legendary",
        "itempool_sr_05_legendary", "itempool_hw_05_legendary", "itempool_shields_05_legendary", "itempool_class_mods_05_legendary",
        "itempool_grenade_gadgets_05_legendary", "itempool_repkit_05_legendary", "itempool_enhancements_05_legendary" };
    const double leg = g_diff.value("legendary", 0.0) * (e.boss ? 5 : 1), loot = g_diff.value("loot", 0.0);
    if (leg > 0 && rnd01() * 100 < leg) {
        if (loot_at(kLeg[(size_t)(rnd01() * 11) % 11], at)) g_diff_stats["legendary"] = g_diff_stats.value("legendary", 0) + 1;
    } else if (loot > 0 && rnd01() * 100 < loot) {
        if (loot_at("itempool_guns_all", at)) g_diff_stats["drops"] = g_diff_stats.value("drops", 0) + 1;
    }
}
static std::vector<std::string> boss_list() {
    static std::vector<std::string> b;
    if (b.empty()) for (auto& n : actor_set()) {
        const bool replay = n.size() > 7 && n.compare(n.size() - 7, 7, "_Replay") == 0 && n.find("Boss") != std::string::npos;
        if (replay || (n.find("BossRift") != std::string::npos && n.find("_Child") == std::string::npos)) b.push_back(n);
    }
    return b;
}
static json diff_boss(const std::string& want) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    const uintptr_t pawn = player_pawn(); V3 p, f; if (!pawn || !actor_loc(pawn, p) || !actor_fwd(pawn, f)) return fail("NO_PAWN", "no player character");
    std::string def;
    if (!want.empty()) def = actor_for(want);
    else { auto b = boss_list(); if (b.empty()) return fail("NO_BOSS", "no boss list"); def = b[(size_t)(rnd01() * b.size()) % b.size()]; }
    if (def.empty()) return fail("NO_ACTOR", "no character matches '" + want + "'");
    const V3 at{ p.x + f.x * 1800, p.y + f.y * 1800, p.z + 80 };
    if (!spawn_char_at(def, at)) return fail("SPAWN", "the game did not spawn " + def);
    g_diff_stats["bosses"] = g_diff_stats.value("bosses", 0) + 1;
    plog("difficulty: boss " + def);
    return ok({ {"boss", def} });
}
static void diff_player() {   // XP / cash live on the Money tab (Att_CombatXP_Multiplier / Att_PickupCash_Multiplier); the
    // Att_PlayerDifficulty_* ones ignore modifiers (computed by the game), so only clean up what an older build left there
    for (const char* n : { "Att_PlayerDifficulty_CombatXP_Multiplier", "Att_PlayerDifficulty_Cash_Multiplier" })
        if (g_mods.count(n)) try { attr_set({ {"name", n}, {"reset", true} }); } catch (...) {}
}
static std::string diff_label() {
    if (!g_diff.value("on", false)) return "Off - the game's own difficulty";
    const int k = g_diff.value("preset", 0); const std::string t = g_diff.value("title", std::string());
    if (!t.empty()) return t + "  (chaos)";
    if (k < 0 || k > 13) return "Custom mix";
    return std::to_string(k) + " - " + kDiffPresets[k].name + "  |  " + kDiffPresets[k].tip;
}
static std::string diff_stats_text() {
    char b[200]; sprintf_s(b, "%d enemies tweaked, %d extra (%d badass), %d drops, %d legendaries, %d bosses",
        g_diff_stats.value("modded", 0), g_diff_stats.value("copies", 0), g_diff_stats.value("badass", 0), g_diff_stats.value("drops", 0),
        g_diff_stats.value("legendary", 0), g_diff_stats.value("bosses", 0));
    return b;
}
// World spawn amount = the game's own spawn director budget (console variable gbx.Spawn.BudgetScale). Verified
// 2026-11-10: same area, scale 1 -> 7-10 enemies, scale 5 -> 22-33. Enemies are spawned by the game itself (real camps).
static double g_budget_applied = -1;
static bool g_budget_touched = false;   // the trainer has set a scale other than 1 in this game run
static void budget_apply() {
    const double want = g_diff.value("on", false) ? std::clamp(g_diff.value("budget", 1.0), 0.05, 20.0) : 1.0;
    if (std::fabs(want - g_budget_applied) < 1e-3) return;
    // never used = never written: the game's own value stays (it was forced to 1.00 on every map load)
    if (!g_budget_touched && std::fabs(want - 1.0) < 1e-3) { g_budget_applied = want; return; }
    const uintptr_t pawn = player_pawn(), pc = player_pc(); if (!pawn || !pc) return;
    static uintptr_t ksl = 0; if (!ksl || !ue::alive(ksl)) ksl = ue::cdo_of(ue::find_class("KismetSystemLibrary"));
    char c[96]; snprintf(c, sizeof c, "gbx.Spawn.BudgetScale %.2f", want);
    if (ue::call_json(ksl, "ExecuteConsoleCommand", { {"WorldContextObject", "addr:" + hexstr(pawn)}, {"Command", c}, {"SpecificPlayer", "addr:" + hexstr(pc)} }, 0).value("ok", false)) {
        g_budget_applied = want; if (std::fabs(want - 1.0) >= 1e-3) g_budget_touched = true; plog(std::string("difficulty: ") + c); }
}
static void diff_tick() {
    static ULONGLONG at = 0; const ULONGLONG now = GetTickCount64();
    if (now - at < 1000) return; at = now;
    { static uintptr_t last_pawn = 0; const uintptr_t pw = player_pawn(); if (pw != last_pawn) { last_pawn = pw; g_budget_applied = -1; } }   // new map / restart
    try { if (session().value("standalone", false)) budget_apply(); } catch (...) {}
    const bool on = g_diff.value("on", false);
    if (!on && g_dene.empty()) return;
    static ULONGLONG solo_at = 0; static bool solo = false;
    if (now - solo_at > 3000) { solo_at = now; solo = session().value("standalone", false); }
    if (!solo) return;
    const uintptr_t pawn = player_pawn(); V3 p; if (!pawn || !actor_loc(pawn, p)) return;
    // sliced character list (no full object walk every second); nothing is judged before the first complete pass
    g_char_scan.want(); if (!g_char_scan.passes) return;
    std::set<uintptr_t> live;
    for (uintptr_t c : g_char_scan.live()) {
        if (c == pawn) continue;
        auto it = g_dene.find(c);
        if (it != g_dene.end() && (it->second.ignore || it->second.dead)) { live.insert(c); continue; }   // nothing to do: no game calls
        V3 v; if (!actor_loc_fast(c, v)) continue;
        if (it == g_dene.end()) {
            const double d = std::sqrt((v.x - p.x) * (v.x - p.x) + (v.y - p.y) * (v.y - p.y) + (v.z - p.z) * (v.z - p.z));
            if (!on || d > 40000) continue;   // 400 m: handled long before you get there
            DiffEnemy e; e.name = base_name(ue::name_of(c));
            e.ignore = e.name.rfind("Char_NPC", 0) == 0 || friendly_team(team_of(c));
            if (!e.ignore) { json h = ue::call_json(c, "IsHealthy", json::object(), 0); if (h.value("ok", false) && h["data"]["ReturnValue"].value("value", 1.0) == 0) e.ignore = true; }
            if (!e.ignore) {
                e.boss = lower(e.name).find("boss") != std::string::npos;
                auto ce = g_clone_expect.find(e.name);
                if (ce == g_clone_expect.end())   // wrapper defs spawn variants: "Char_GunToterAssault" -> "..._Female"
                    for (auto x = g_clone_expect.begin(); x != g_clone_expect.end(); ++x) if (e.name.rfind(x->first + "_", 0) == 0) { ce = x; break; }
                if (ce != g_clone_expect.end() && ce->second > 0) { e.clone = true; if (--ce->second <= 0) g_clone_expect.erase(ce); }
                else if (!e.boss) diff_clone(e.name, v, p);
            }
            it = g_dene.emplace(c, e).first; g_dene_dirty = true;
        }
        live.insert(c);
        DiffEnemy& e = it->second;
        if (e.ignore || e.dead) continue;
        g_last_hostile = now;
        if (e.gen != g_diff_gen) diff_mod(c, e);
        json h = ue::call_json(c, "IsHealthy", json::object(), 0);
        if (h.value("ok", false) && h["data"]["ReturnValue"].value("value", 1.0) == 0) { e.dead = true; g_dene_dirty = true; if (on) diff_drop(v, e); }
    }
    for (auto i = g_dene.begin(); i != g_dene.end();) { if (live.count(i->first)) ++i; else { i = g_dene.erase(i); g_dene_dirty = true; } }
    if (!on) { if (!g_dene.empty()) g_dene_dirty = true; g_dene.clear(); g_clone_expect.clear(); }
    if (g_dene_dirty) { g_dene_dirty = false; try { diff_live_save(); } catch (...) {} }
    if (!on) return;
    const double bm = g_diff.value("boss_minutes", 0.0);
    if (bm > 0) {
        if (!g_boss_next) g_boss_next = now + (ULONGLONG)(bm * 60000);
        else if (now >= g_boss_next) {
            g_boss_next = now + (ULONGLONG)(bm * 60000);
            if (now - g_last_hostile < 120000) diff_boss("");   // only out in the world (enemies seen in the last 2 minutes), not in town
        }
    } else g_boss_next = 0;
}
static json diff_state() {
    json o = g_diff; o["label"] = diff_label(); o["stats"] = g_diff_stats; o["tracked"] = (int)g_dene.size();
    json pr = json::array(); for (int i = 0; i < 14; ++i) pr.push_back({ {"level", i}, {"name", kDiffPresets[i].name}, {"tip", kDiffPresets[i].tip} });
    o["presets"] = pr; o["bosses"] = boss_list().size();
    return ok(o);
}
static void diff_load_preset(int k) {
    const DiffPreset& d = kDiffPresets[k];
    g_diff["health"] = d.hp; g_diff["damage"] = d.dmg; g_diff["speed"] = d.spd; g_diff["reflex"] = d.rfx; g_diff["accuracy"] = d.acc;
    g_diff["fire_rate"] = d.fire; g_diff["extra"] = d.extra; g_diff["badass"] = d.badass; g_diff["loot"] = d.loot; g_diff["legendary"] = d.leg;
    g_diff["xp"] = d.xp; g_diff["cash"] = d.cash; g_diff["arm"] = 0.0;
    static const double kBudget[14] = { 1, 0.5, 0.6, 0.7, 0.8, 1, 1, 1.25, 1.5, 1.75, 2, 2.5, 3, 4 };
    g_diff["budget"] = kBudget[k]; g_diff["boss_minutes"] = d.boss; g_diff["preset"] = k; g_diff["on"] = k != 0; g_diff["title"] = "";
}
static json diff_cmd(const json& a) {
    if (a.contains("boss")) return diff_boss(a["boss"].is_string() ? a["boss"].get<std::string>() : std::string());
    if (a.value("bosses", false)) return ok(boss_list());
    const json diff_before = g_diff;   // a refused change (not solo) must not stay in memory
    bool changed = false;
    auto as_num = [](const json& v, double& out) -> bool {
        if (v.is_number()) { out = v.get<double>(); return true; }
        if (v.is_string()) { char* e = nullptr; const std::string s = v.get<std::string>(); out = strtod(s.c_str(), &e); return e && e != s.c_str(); }
        return false;
    };
    if (a.contains("preset")) { double k = 0; if (!as_num(a["preset"], k)) return fail("BAD_ARG", "preset 0..13");
        if (k >= 0) { diff_load_preset(std::clamp((int)k, 0, 13)); changed = true; } }   // -1 = "Custom" entry: keep the current mix
    if (a.value("reset", false)) { diff_load_preset(0); changed = true; }
    if (a.value("chaos", false)) {   // random hard-ish mix with a random funny name
        diff_load_preset(7 + (int)(rnd01() * 7) % 7);
        for (const char* k : { "health", "damage", "speed", "accuracy", "fire_rate" }) g_diff[k] = std::round(g_diff.value(k, 1.0) * (0.7 + rnd01() * 0.7) * 100) / 100;
        g_diff["extra"] = std::round(rnd01() * 25) / 10; g_diff["badass"] = std::round(rnd01() * 50);
        g_diff["title"] = kChaosNames[(size_t)(rnd01() * 20) % 20]; g_diff["preset"] = -1; changed = true;
    }
    if (a.contains("key")) {
        const std::string k = a.value("key", std::string());
        if (!g_diff_def.contains(k) || k == "on" || k == "preset" || k == "title") return fail("BAD_KEY", k);
        double v = 0; if (!as_num(a.value("v", json()), v)) return fail("NEEDS_VALUE", "v = number");
        g_diff[k] = v; g_diff["on"] = true; g_diff["preset"] = -1; g_diff["title"] = ""; changed = true;
    }
    if (a.contains("on") && a["on"].is_boolean()) { g_diff["on"] = a["on"].get<bool>(); changed = true; }
    if (changed) {
        json s = session(); if (!s.value("standalone", false)) { g_diff = diff_before; return fail("NOT_SOLO", "solo only"); }
        ++g_diff_gen; g_boss_next = 0; diff_player();
        std::ofstream(diff_file()) << g_diff.dump(1);
        { std::lock_guard<std::mutex> lk(g_mtx); g_diff_snap = g_diff; ++g_ver; }
        plog("difficulty: " + diff_label());
    }
    return diff_state();
}


// ------------------------------------------------------------------ CAMERA (BL.CAMERA, panel tab "Camera")
// third person / orbit = the game's own camera modes: PlayerCameraModeManager::SetActorCameraMode(pawn, "ThirdPerson" |
// "ThirdPersonOrbit" | "Default") on the OakPlayerCameraManager. HUD = HUD::ShowHUD (a toggle; bShowHUD read first).
// Free camera = our own CameraActor (GameplayStatics BeginDeferredActorSpawnFromClass + FinishSpawningActor) made the view
// target (PlayerController::SetViewTargetWithBlend); it follows the control rotation (mouse) and flies with W/A/S/D,
// Space / C (up/down), Shift (x4); the player character is frozen (CustomTimeDilation 0) while it is on.
// FOV: the camera mode takes FOV from the user settings every frame (SetBaseFOV / mode SetBaseFOV / PlayerController::fov
// had no effect), so the trainer writes CameraCachePrivate.POV.FOV after the game every tick; the free cam uses its own lens.
// Photo freeze = GameplayStatics::SetGlobalTimeDilation(0.0001) (the free camera still moves: it is moved by teleport).
static double prop_n(uintptr_t o, const char* f);
static json g_cam_snap = json::object();
static json g_cam = { {"mode", "Default"}, {"hud", true}, {"free", false}, {"fov", 0.0}, {"speed", 1.0}, {"freeze", false},
    {"matrix", false}, {"bullet", false}, {"matrix_slow", 0.2}, {"matrix_tint", true},
    {"rain", true}, {"rain_density", 1.3}, {"rain_speed", 1.0}, {"rain_alpha", 0.9}, {"rain_size", 24.0},
    {"tp", true}, {"tp_dist", 230.0}, {"tp_side", 65.0}, {"tp_height", -15.0}, {"tp_left", false}, {"tp_aim", true}, {"tp_aim_dist", 120.0}, {"tp_aim_side", 50.0}, {"tp_aim_fov", 60.0} };
// MATRIX code rain: drawn by the overlay (wb::hud_draw, render thread) from these copies - same procedural rain as the
// Unreal plugin's RipXicMatrixView post-process (per-column seed, speed, fading trail, bright head, glyphs that flicker).
struct RainCfg { std::atomic<bool> on{ false }, tint{ false }; std::atomic<float> density{ 1.3f }, speed{ 1.f }, alpha{ 0.9f }, size{ 24.f }; };
RainCfg g_rain;
std::atomic<bool> g_tp_xhair{ false };   // third-person aim: the game hides its crosshair while zoomed, we draw one
static void rain_sync() {
    g_rain.on = g_cam.value("matrix", false) && g_cam.value("rain", true);
    g_rain.tint = g_cam.value("matrix", false) && g_cam.value("matrix_tint", true);
    g_rain.density = (float)g_cam.value("rain_density", 1.3); g_rain.speed = (float)g_cam.value("rain_speed", 1.0);
    g_rain.alpha = (float)g_cam.value("rain_alpha", 0.9); g_rain.size = (float)g_cam.value("rain_size", 24.0);
}
static void cam_snap() { std::lock_guard<std::mutex> lk(g_mtx); g_cam_snap = g_cam; ++g_ver; }   // panel copy of g_cam
static uintptr_t g_cam_actor = 0; static V3 g_cam_pos; static ULONGLONG g_cam_at = 0;
static uintptr_t g_cam_pawn = 0;              // the pawn the free / matrix camera was started on
static bool g_cam_restore_pending = false;    // cam_restore_all had no world to work in yet (no pawn / controller): retried by camera_tick
static json g_cam_saved;
static bool g_tp_applied = false;             // third person: our offsets are in the game's camera definition
static void tp_settings_save();
// rotator fields come back as "Pitch"/"Yaw" or "pitch"/"yaw" depending on the call path - read either
static double rot_get(const json& v, const char* k) {
    if (v.contains(k)) return v.value(k, 0.0);
    std::string l = k; for (auto& c : l) c = (char)tolower((unsigned char)c); return v.value(l, 0.0);
}
static uintptr_t cam_mgr() { return prop_obj(player_pc(), "PlayerCameraManager"); }
static bool cam_set_mode(const std::string& m, double blend = 0.35, bool force = false) {
    const uintptr_t cm = cam_mgr(), pawn = player_pawn(); if (!cm || !pawn) return false;
    return ue::call_json(cm, "SetActorCameraMode", { {"Actor", "addr:" + hexstr(pawn)}, {"ModeName", "name:" + m}, {"OptionalTransitionName", "name:None"},
        {"BlendTimeOverride", blend}, {"bTeleport", false}, {"bForceResetMode", force} }, 0).value("ok", false);
}
static bool cam_hud(bool on) {
    const uintptr_t hud = prop_obj(player_pc(), "MyHUD"); if (!hud) return false;
    if ((prop_n(hud, "bShowHUD") > 0) == on) return true;
    return ue::call_json(hud, "ShowHUD", json::object(), 0).value("ok", false);
}
static bool cam_view_point(V3& pos, double& pitch, double& yaw) {
    const uintptr_t cm = cam_mgr(); if (!cm) return false;
    uintptr_t b = 0; ue::Prop leaf;
    if (!ue::resolve(cm, ue::class_of(cm), "CameraCachePrivate.POV.Location", b, leaf)) return false;
    double v[3]; if (!ue::mread(b + leaf.offset, v, sizeof v)) return false; pos = { v[0], v[1], v[2] };
    if (ue::resolve(cm, ue::class_of(cm), "CameraCachePrivate.POV.Rotation", b, leaf)) { double r[3]; if (ue::mread(b + leaf.offset, r, sizeof r)) { pitch = r[0]; yaw = r[1]; } }
    return true;
}
static void cam_lens(double fov) {   // free camera lens
    if (!g_cam_actor || !ue::alive(g_cam_actor)) return;
    const uintptr_t comp = prop_obj(g_cam_actor, "CameraComponent"); if (comp) ue::call_json(comp, "SetFieldOfView", { {"InFieldOfView", fov > 0 ? fov : 90.0} }, 0);
}
static json cam_free(bool on) {
    const uintptr_t pc = player_pc(), pawn = player_pawn(); if (!pc || !pawn) return fail("NO_PAWN", "load into the game first");
    if (on) {
        if (g_cam_actor && ue::alive(g_cam_actor)) return ok(g_cam);
        V3 p; double pitch = 0, yaw = 0; if (!cam_view_point(p, pitch, yaw)) { actor_loc(pawn, p); p.z += 180; }
        { const double yr = yaw * 3.14159265 / 180; p.x -= std::cos(yr) * 150; p.y -= std::sin(yr) * 150; p.z += 50; }   // a step behind your head, not inside it
        static uintptr_t gs = 0; if (!gs || !ue::alive(gs)) gs = ue::cdo_of(ue::find_class("GameplayStatics"));
        const uintptr_t cls = ue::find_class("CameraActor"); if (!cls) return fail("NO_CLASS", "CameraActor class missing");
        json tr = { {"Rotation", { {"W", 1.0} }}, {"Translation", { {"X", p.x}, {"Y", p.y}, {"Z", p.z} }}, {"Scale3D", { {"X", 1.0}, {"Y", 1.0}, {"Z", 1.0} }} };
        json r = ue::call_json(gs, "BeginDeferredActorSpawnFromClass", { {"WorldContextObject", "addr:" + hexstr(pawn)}, {"ActorClass", "addr:" + hexstr(cls)},
            {"SpawnTransform", tr}, {"CollisionHandlingOverride", 1} }, 0);
        if (!r.value("ok", false)) return fail("SPAWN", r.value("message", std::string("BeginDeferredActorSpawnFromClass failed")));
        const uintptr_t act = std::stoull(r["data"]["ReturnValue"].value("addr", std::string("0x0")), nullptr, 16); if (!act) return fail("SPAWN", "no camera actor");
        ue::call_json(gs, "FinishSpawningActor", { {"Actor", "addr:" + hexstr(act)}, {"SpawnTransform", tr} }, 0);
        g_cam_actor = act; g_cam_pos = p; g_cam_pawn = pawn;
        ue::call_json(pc, "SetViewTargetWithBlend", { {"NewViewTarget", "addr:" + hexstr(act)}, {"BlendTime", 0.25}, {"BlendFunc", 0}, {"BlendExp", 0.0}, {"bLockOutgoing", false} }, 0);
        std::string w; write_field(pawn, "CustomTimeDilation", 0.0, &w);
        cam_lens(g_cam.value("fov", 0.0));
        g_cam["free"] = true; g_cam_at = GetTickCount64(); plog("camera: free camera on");
    } else {
        ue::call_json(pc, "SetViewTargetWithBlend", { {"NewViewTarget", "addr:" + hexstr(pawn)}, {"BlendTime", 0.25}, {"BlendFunc", 0}, {"BlendExp", 0.0}, {"bLockOutgoing", false} }, 0);
        std::string w; write_field(pawn, "CustomTimeDilation", 1.0, &w);
        if (g_cam_actor && ue::alive(g_cam_actor)) ue::call_json(g_cam_actor, "K2_DestroyActor", json::object(), 0);   // our own actor: safe to destroy
        g_cam_actor = 0; g_cam["free"] = false; plog("camera: free camera off");
    }
    rain_sync();
    { std::lock_guard<std::mutex> lk(g_mtx); g_cam_snap = g_cam; ++g_ver; }
    return ok(g_cam);
}
static void cam_freeze(bool on) {
    static uintptr_t gs = 0; if (!gs || !ue::alive(gs)) gs = ue::cdo_of(ue::find_class("GameplayStatics"));
    const uintptr_t pawn = player_pawn(); if (!pawn) return;
    // unfreezing during bullet time goes back to bullet time, not to full speed (the player runs at 1/slow there)
    const double back = g_cam.value("bullet", false) ? std::clamp(g_cam.value("matrix_slow", 0.2), 0.05, 1.0) : 1.0;
    ue::call_json(gs, "SetGlobalTimeDilation", { {"WorldContextObject", "addr:" + hexstr(pawn)}, {"TimeDilation", on ? 0.0001 : back} }, 0);
    g_cam["freeze"] = on;
}

// MATRIX view (2026-11-18, user: "just an effect on the screen"): the green colour grade + the code rain, nothing else.
// It does not touch the camera or the player, so it works in first person, the trainer's third person and the free camera.
// Both are drawn by the overlay (render thread): a green screen tint + vignette, and the code rain. A PostProcessVolume
// colour grade was tried first: spawned, unbound, top priority, values set - Borderlands' renderer showed no change.
// Bullet time is its own switch (BL.CAMERA {bullet}): world SetGlobalTimeDilation slow, the player's CustomTimeDilation
// = 1/slow so you keep normal speed. It works with or without the Matrix look.
static bool line_hit(const V3& a, const V3& b, V3& hitp);
static uintptr_t g_mx_ppv = 0;                // the Matrix colour grade volume (our own actor)
static uintptr_t g_bullet_pawn = 0;           // the pawn bullet time gave 1/slow time (a respawned pawn gets it again)
static bool set_struct_num(uintptr_t obj, const std::string& path, double v) {
    uintptr_t b = 0; ue::Prop leaf; if (!ue::resolve(obj, ue::class_of(obj), path, b, leaf)) return false; return ue::set_num(b, leaf, v);
}
static bool write_field(uintptr_t o, const std::string& field, double v, std::string* why);
static void matrix_tint(bool on) {   // the tint is drawn by the overlay (rain_sync); this only removes an old tint volume
    if (g_mx_ppv && ue::alive(g_mx_ppv)) ue::call_json(g_mx_ppv, "K2_DestroyActor", json::object(), 0);   // our own actor
    g_mx_ppv = 0;
    (void)on; rain_sync();
}
static void bullet_time(bool on) {
    static uintptr_t gs = 0; if (!gs || !ue::alive(gs)) gs = ue::cdo_of(ue::find_class("GameplayStatics"));
    const uintptr_t pawn = player_pawn(); if (!pawn || !gs) return;
    const double slow = on ? std::clamp(g_cam.value("matrix_slow", 0.2), 0.05, 1.0) : 1.0;
    if (!g_cam.value("freeze", false)) ue::call_json(gs, "SetGlobalTimeDilation", { {"WorldContextObject", "addr:" + hexstr(pawn)}, {"TimeDilation", slow} }, 0);
    std::string w; if (!g_cam.value("free", false)) write_field(pawn, "CustomTimeDilation", 1.0 / slow, &w);   // you keep (almost) normal speed
    g_bullet_pawn = on ? pawn : 0;
    g_cam["bullet"] = on;
}
// Everything the camera features change in the game, put back in one place: world time 1, player time 1, view back on the
// player, our camera actor gone, flags off. Used when the Matrix / free camera loses its actor or its pawn (death, respawn,
static void cam_restore_all(const char* why) {
    const uintptr_t pc = player_pc(), pawn = player_pawn(), ctx = pawn ? pawn : pc;
    g_cam_restore_pending = !ctx;   // nothing to call yet: camera_tick tries again
    static uintptr_t gs = 0; if (!gs || !ue::alive(gs)) gs = ue::cdo_of(ue::find_class("GameplayStatics"));
    if (ctx) ue::call_json(gs, "SetGlobalTimeDilation", { {"WorldContextObject", "addr:" + hexstr(ctx)}, {"TimeDilation", 1.0} }, 0);
    if (pawn) {
        std::string w; write_field(pawn, "CustomTimeDilation", 1.0, &w);
        if (pc) ue::call_json(pc, "SetViewTargetWithBlend", { {"NewViewTarget", "addr:" + hexstr(pawn)}, {"BlendTime", 0.25}, {"BlendFunc", 0}, {"BlendExp", 0.0}, {"bLockOutgoing", false} }, 0);
    }
    if (g_cam_actor && ue::alive(g_cam_actor)) ue::call_json(g_cam_actor, "K2_DestroyActor", json::object(), 0);   // our own actor
    g_cam_actor = 0; g_cam_pawn = 0;
    g_cam["bullet"] = false; g_bullet_pawn = 0; g_cam["free"] = false; g_cam["freeze"] = false;
    rain_sync(); cam_snap();
    try { tp_settings_save(); } catch (...) {}
    plog(std::string("camera: restored (") + why + ")");
}
static void cam_recover() {
    const json lv = g_cam_saved; g_cam_saved = json();
    try {
        if (!lv.is_object()) return;
        const bool mx = lv.value("matrix", false), fr = lv.value("free", false), fz = lv.value("freeze", false), bt = lv.value("bullet", false);
        const std::string mode = lv.value("mode", std::string("Default"));
        g_cam["mode"] = mode;
        if (mx) {   // the Matrix look stays on: adopt the volume (a second one would stay green after "off")
            const uintptr_t v = std::stoull(lv.value("ppv", std::string("0")), nullptr, 16);
            if (v && ue::alive(v) && ue::name_of(ue::class_of(v)) == "PostProcessVolume") g_mx_ppv = v;
            g_cam["matrix"] = true; matrix_tint(true); }
        if (fr || fz || bt) {
            const uintptr_t act = std::stoull(lv.value("actor", std::string("0")), nullptr, 16);
            if (act && ue::alive(act) && ue::name_of(ue::class_of(act)) == "CameraActor") g_cam_actor = act;
            cam_restore_all("left on by the previous trainer copy");
        } else {
            if (mode == "ThirdPerson") g_tp_applied = false;   // tp_tick finds the cached definition and applies the saved settings
            cam_snap();
        }
    } catch (...) {}
}
static json cam_matrix(bool on) {
    if (!player_pawn()) return fail("NO_PAWN", "load into the game first");
    g_cam["matrix"] = on;
    matrix_tint(on && g_cam.value("matrix_tint", true));
    plog(on ? "camera: matrix look on" : "camera: matrix look off");
    return ok(g_cam);
}
static void matrix_tick() {   // bullet time follows a respawn (the new character gets normal speed again)
    static ULONGLONG at = 0; const ULONGLONG now = GetTickCount64();
    if (now - at < 1000) return; at = now;
    if (g_cam.value("bullet", false)) { const uintptr_t pw = player_pawn(); if (pw && pw != g_bullet_pawn) bullet_time(true); }
}


// ------------------------------------------------------------------ NATIVE THIRD PERSON
// The game's camera modes are data: the "ThirdPerson" mode has a CameraModeBehavior_RelativeOffset {vtable, FName label,
// LocationOffset (-256, 0, -25), RotationOffset}. The live mode copies its behaviours when it starts, so we edit that
// definition in memory (no files touched) and restart the mode with bForceResetMode + a blend -> the game's own camera
// (its collision, crouch, blends) now sits over the shoulder. Aim zoom: right mouse pulls the camera in + lowers the FOV
// (the game zooms but shows no sight in this mode, so we draw a reticle). Weapons trace from the camera view point, so shots go to the crosshair.
// The definition is found by an in-process memory scan (background thread) then identified by restarting ThirdPerson
// with a tiny different Y on each candidate. Restored to (-256, 0, -25) when third person is switched off.
static const double kTpOrig[3] = { -256.0, 0.0, -25.0 };
static double g_tp_orig[6] = { -256.0, 0.0, -25.0, 0.0, 0.0, 0.0 };   // what the definition held before we edited it: LocationOffset + RotationOffset
static uintptr_t g_tp_def = 0, g_tp_vt = 0, g_tp_label = 0;       // def = address of LocationOffset
static std::atomic<int> g_tp_scan{ 0 };                            // 0 idle, 1 scanning, 2 scan done, 3 ready, 4 failed
static std::vector<uintptr_t> g_tp_cands; static std::mutex g_tp_mtx;
static int g_tp_cal_step = 0, g_tp_cal_tries = 0; static ULONGLONG g_tp_cal_at = 0, g_tp_cal_next = 0; static bool g_tp_aiming = false, g_tp_cal_by_vt = false;
static uintptr_t g_tp_pawn = 0;   // the pawn third person was last applied on (a new pawn starts in the game's own mode)
struct TpCand { uintptr_t at = 0; uint64_t vt = 0, label = 0; double orig[6] = {}; double y = 0; };
static std::vector<TpCand> g_tp_cal;   // calibration in progress: re-validated candidates with their original values
static bool tp_sane(const double* d, int n) { for (int i = 0; i < n; ++i) if (!std::isfinite(d[i]) || std::fabs(d[i]) > 5000.0) return false; return true; }
static double g_tp_fov = 0; static ULONGLONG g_tp_tick_at = 0; static std::string g_tp_msg = "not started";
static void tp_scan_region(uintptr_t b, uintptr_t e, uintptr_t gb, uintptr_t ge, uintptr_t want_vt, uintptr_t want_label, std::vector<uintptr_t>& out) {
    __try {
        const uint64_t p0 = 0xC070000000000000ull, p2 = 0xC039000000000000ull;   // -256.0, -25.0
        for (uintptr_t a = b + 0x18; a + 0x18 <= e; a += 8) {
            const uint64_t* q = (const uint64_t*)a; const uint64_t vt = *(const uint64_t*)(a - 0x18);
            if (want_vt) { if (vt != want_vt || *(const uint64_t*)(a - 0x10) != want_label) continue; }
            else { if (q[0] != p0 || q[1] != 0 || q[2] != p2 || vt < gb || vt >= ge) continue; }
            out.push_back(a); if (out.size() > 64) return;
        }
    } __except (1) {}
}
static void tp_scan_thread(uintptr_t want_vt, uintptr_t want_label) {
    size_t gsz = 0; const uintptr_t gb = game_base(&gsz), ge = gb + (gsz ? gsz : 0x40000000);
    std::vector<uintptr_t> out; MEMORY_BASIC_INFORMATION mbi; uintptr_t p = 0x10000;
    while (p < 0x7FF000000000ull && VirtualQuery((void*)p, &mbi, sizeof mbi)) {
        const uintptr_t b = (uintptr_t)mbi.BaseAddress, e = b + mbi.RegionSize;
        if (mbi.State == MEM_COMMIT && mbi.Type == MEM_PRIVATE && mbi.Protect == PAGE_READWRITE) tp_scan_region(b, e, gb, ge, want_vt, want_label, out);
        if (out.size() > 64) break;
        p = e;
    }
    { std::lock_guard<std::mutex> lk(g_tp_mtx); g_tp_cands = out; }
    g_tp_scan = 2;
}
static bool tp_write(uintptr_t at, double x, double y, double z) { const double v[3] = { x, y, z }; return ue::mwrite(at, v, sizeof v); }
static bool tp_state_offset(double out[3]) {
    const uintptr_t st = prop_obj(cam_mgr(), "CameraModeState"); if (!st) return false;
    uintptr_t b = 0; ue::Prop leaf; if (!ue::resolve(st, ue::class_of(st), "CameraLocationOffset", b, leaf)) return false;
    return ue::mread(b + leaf.offset, out, sizeof(double) * 3);
}
static const char* kTpKeys[] = { "tp", "tp_dist", "tp_side", "tp_height", "tp_left", "tp_aim", "tp_aim_dist", "tp_aim_side", "tp_aim_fov", "tp_converge" };
static void tp_settings_save() {
    if (g_data_dir.empty()) return;
    json j = json::object(); for (auto k : kTpKeys) if (g_cam.contains(k)) j[k] = g_cam[k];
    j["live"] = { {"pid", (uint64_t)GetCurrentProcessId()}, {"mode", g_cam.value("mode", std::string("Default"))}, {"matrix", g_cam.value("matrix", false)},
                  {"free", g_cam.value("free", false)}, {"freeze", g_cam.value("freeze", false)}, {"bullet", g_cam.value("bullet", false)},
                  {"ppv", hexstr(g_mx_ppv)}, {"actor", hexstr(g_cam_actor)} };
    std::ofstream(g_data_dir + "\\camera.json") << j.dump(1);
}
static void tp_settings_load() { static bool done = false; if (done || g_data_dir.empty()) return; done = true;
    if (PadShare* ps = pad_share()) ps->mask = 0;   // a hidden L3 the previous copy left set
    try {
        json j = read_json(g_data_dir + "\\camera.json"); if (!j.is_object()) return;
        for (auto k : kTpKeys) if (j.contains(k)) {   // only values of the right type (a wrong one made every later read throw)
            const bool flag = !strcmp(k, "tp") || !strcmp(k, "tp_left") || !strcmp(k, "tp_aim");
            if (flag ? j[k].is_boolean() : j[k].is_number()) g_cam[k] = j[k]; }
        if (j.contains("live") && j["live"].is_object() && j["live"].contains("pid") && j["live"]["pid"].is_number() &&
            j["live"]["pid"].get<uint64_t>() == (uint64_t)GetCurrentProcessId()) g_cam_saved = j["live"];
    } catch (...) {}
}
static void tp_cache_save() {
    std::ofstream(g_data_dir + "\\tp_cache.json") << json{ {"pid", (uint64_t)GetCurrentProcessId()}, {"vt", hexstr(g_tp_vt)}, {"label", hexstr(g_tp_label)}, {"at", hexstr(g_tp_def)},
        {"orig", std::vector<double>(g_tp_orig, g_tp_orig + 6)} }.dump();
}
static bool tp_valid() {   // still the same object (vtable + label) holding plausible offsets
    if (!g_tp_def || !g_tp_vt) return false; uint64_t vt = 0, lb = 0; double d[6];
    return ue::rd_as(g_tp_def - 0x18, vt) && ue::rd_as(g_tp_def - 0x10, lb) && vt == g_tp_vt && lb == g_tp_label && ue::mread(g_tp_def, d, sizeof d) && tp_sane(d, 6);
}
// <data>\tp_cache.json from this game run: true = the definition is known and valid (state 3). vt/label are returned even
// when the address is stale, for the quick vtable+label rescan.
static bool tp_load_cache(uintptr_t* vt_out = nullptr, uintptr_t* lb_out = nullptr) {
    try {
        json c = read_json(g_data_dir + "\\tp_cache.json");
        if (!c.is_object() || !c.contains("pid") || !c["pid"].is_number() || c["pid"].get<uint64_t>() != (uint64_t)GetCurrentProcessId()) return false;
        const uintptr_t vt = std::stoull(c.value("vt", std::string("0")), nullptr, 16), lb = std::stoull(c.value("label", std::string("0")), nullptr, 16);
        const uintptr_t at = std::stoull(c.value("at", std::string("0")), nullptr, 16);
        if (c.contains("orig") && c["orig"].is_array() && c["orig"].size() == 6) {
            double o[6]; bool num = true; for (int i = 0; i < 6; ++i) { if (!c["orig"][i].is_number()) { num = false; break; } o[i] = c["orig"][i].get<double>(); }
            if (num && tp_sane(o, 6)) memcpy(g_tp_orig, o, sizeof o);
        }
        if (vt_out) *vt_out = vt; if (lb_out) *lb_out = lb;
        g_tp_def = at; g_tp_vt = vt; g_tp_label = lb;
        if (tp_valid()) { g_tp_scan = 3; g_tp_msg = "ready (cached)"; return true; }
    } catch (...) {}
    g_tp_def = 0; return false;
}
static void tp_start_scan() {
    if (g_tp_scan == 1) return;
    uintptr_t vt = 0, lb = 0;
    if (tp_load_cache(&vt, &lb)) return;
    if (!vt) { g_tp_vt = g_tp_label = 0; }
    g_tp_cal_by_vt = vt != 0; g_tp_cal_step = 0; g_tp_cal.clear();
    g_tp_scan = 1; g_tp_msg = "finding the game's third-person camera...";
    std::thread(tp_scan_thread, vt, lb).detach();
}
struct TpMeas { bool ok = false; double lat = 0, vert = 0, back = 0; double key = 0; };
static TpMeas g_tp_meas[2]; static ULONGLONG g_tp_applied_at = 0; static int g_tp_iter = 0;
static double tp_key(bool aim) {   // the settings a measurement belongs to
    return (aim ? g_cam.value("tp_aim_side", 50.0) * 7.0 + g_cam.value("tp_aim_dist", 120.0) * 13.0 : g_cam.value("tp_side", 65.0) * 7.0 + g_cam.value("tp_dist", 230.0) * 13.0)
         + g_cam.value("tp_height", -15.0) * 17.0 + (g_cam.value("tp_left", false) ? 100000.0 : 0.0);
}
static void tp_apply(double blend) {
    if (g_tp_scan != 3 || !tp_valid()) return;
    const bool aim = g_tp_aiming && g_cam.value("tp_aim", true);
    const double side = (aim ? g_cam.value("tp_aim_side", 50.0) : g_cam.value("tp_side", 65.0)) * (g_cam.value("tp_left", false) ? -1 : 1);
    const double dist = aim ? g_cam.value("tp_aim_dist", 120.0) : g_cam.value("tp_dist", 230.0);
    const double height = g_cam.value("tp_height", -15.0) + (aim ? 5.0 : 0.0);
    tp_write(g_tp_def, -dist, side, height);
    // The game's use / pick-up check runs from the character's eyes along the view direction, the crosshair ray from
    // the camera (beside and behind the head). Turn the camera slightly inward so both rays meet `tp_converge` cm in
    // front of the eyes: what is under the crosshair at pick-up range is what the use check sees.
    // RotationOffset (Pitch, Yaw, Roll) follows LocationOffset in CameraModeBehavior_RelativeOffset.
    const double conv = std::max(50.0, g_cam.value("tp_converge", 350.0)), deg = 180.0 / 3.14159265358979;
    double lat = side, vert = height, back = dist;   // estimate; replaced by what tp_measure() saw for these settings
    const TpMeas& m = g_tp_meas[aim ? 1 : 0]; if (m.ok && m.key == tp_key(aim)) { lat = m.lat; vert = m.vert; back = m.back; }
    const double rot[3] = { -std::atan2(vert, back + conv) * deg, -std::atan2(lat, back + conv) * deg, 0.0 };   // below the eyes -> tilt up
    ue::mwrite(g_tp_def + 0x18, rot, sizeof rot);
    g_tp_applied = cam_set_mode("ThirdPerson", blend, true);   // false (no camera manager yet): tp_tick tries again
    g_tp_pawn = player_pawn(); g_tp_applied_at = GetTickCount64();
}
// where the camera really is, in the frame of the control rotation, relative to the eyes (the anchor is not the eyes)
static bool tp_measure(TpMeas& out) {
    const uintptr_t cm = cam_mgr(), pawn = player_pawn(), pc = player_pc(); if (!cm || !pawn || !pc) return false;
    uintptr_t b = 0; ue::Prop leaf; double cl[3];
    if (!ue::resolve(cm, ue::class_of(cm), "CameraCachePrivate.POV.Location", b, leaf) || !ue::mread(b + leaf.offset, cl, sizeof cl)) return false;
    json e = ue::call_json(pawn, "GetActorEyesViewPoint", json::object(), 0); if (!e.value("ok", false)) return false;
    const json el = e["data"]["OutLocation"]["value"];   // keys X / Y / Z
    json cr = ue::call_json(pc, "GetControlRotation", json::object(), 0); if (!cr.value("ok", false)) return false;
    const json rv = cr["data"]["ReturnValue"]["value"];
    const double yaw = (rv.contains("yaw") ? rv.value("yaw", 0.0) : rv.value("Yaw", 0.0)) * 3.14159265358979 / 180.0;
    const double dx = cl[0] - el.value("X", 0.0), dy = cl[1] - el.value("Y", 0.0), dz = cl[2] - el.value("Z", 0.0);
    out.back = -(dx * std::cos(yaw) + dy * std::sin(yaw)); out.lat = -dx * std::sin(yaw) + dy * std::cos(yaw); out.vert = dz; out.ok = true;
    return std::fabs(out.lat) < 600 && out.back > -100 && out.back < 1200;
}
static void tp_restore() {
    if (g_tp_scan == 3 && tp_valid()) { tp_write(g_tp_def, g_tp_orig[0], g_tp_orig[1], g_tp_orig[2]); ue::mwrite(g_tp_def + 0x18, g_tp_orig + 3, sizeof(double) * 3); }
    g_tp_applied = false; g_tp_aiming = false; g_tp_fov = 0; g_tp_xhair = false;
    if (PadShare* ps = pad_share()) ps->mask = 0;
}
// third person is not being ticked (Matrix / free camera, another mode, job paused, no pawn, still scanning): no reticle, no
// hidden L3, no aim state left behind. The aim offsets are re-applied as hip-fire when tp_tick runs again.
static void tp_idle() {
    if (!g_tp_aiming && !g_tp_xhair.load() && g_tp_fov <= 0) return;
    if (g_tp_aiming) g_tp_applied = false;
    g_tp_aiming = false; g_tp_xhair = false; g_tp_fov = 0;
    if (PadShare* ps = pad_share()) ps->mask = 0;
}
// calibration could not identify the definition: cached vtable/label -> one full pattern scan; else third person stays off
// until it is switched on again (camera_cmd clears state 4)
static void tp_cal_give_up(const char* why) {
    g_tp_cal.clear(); g_tp_cal_step = 0; g_tp_cal_tries = 0;
    if (g_tp_cal_by_vt) {
        DeleteFileA((g_data_dir + "\\tp_cache.json").c_str()); g_tp_vt = g_tp_label = 0; g_tp_cal_by_vt = false; g_tp_scan = 0;
        plog(std::string("tp: ") + why + " (from cached camera data) - doing a full scan"); return;
    }
    g_tp_scan = 4; g_tp_msg = why; plog(std::string("tp: ") + why);
}
// main thread, every tick while third person is on
static void tp_tick(double& fov_out) {
    tp_settings_load();
    if (!g_cam.value("tp", true)) { if (g_tp_applied) { tp_restore(); cam_set_mode("ThirdPerson", 0.3, true); } tp_idle(); return; }
    const int st = g_tp_scan.load();
    if (st != 3) tp_idle();
    if (st == 0) { tp_start_scan(); return; }
    if (st == 1 || st == 4) return;
    const ULONGLONG now = GetTickCount64();
    if (st == 2) {   // calibrate: give every candidate a tiny different Y, restart ThirdPerson, see which one the camera uses
        if (g_tp_cal_step == 0) {
            if (now < g_tp_cal_next) return;   // pause between attempts
            std::vector<uintptr_t> c; { std::lock_guard<std::mutex> lk(g_tp_mtx); c = g_tp_cands; }
            // The scan ran on another thread a moment ago: every candidate is checked again right before it is written
            // (vtable inside the game image, cached vtable+label or the exact original offsets, plausible numbers) and
            // its six doubles are remembered so exactly those go back.
            size_t gsz = 0; const uintptr_t gb = game_base(&gsz), ge = gb + (gsz ? gsz : 0x40000000);
            g_tp_cal.clear();
            for (uintptr_t at : c) {
                TpCand k; k.at = at;
                if (!ue::rd_as(at - 0x18, k.vt) || !ue::rd_as(at - 0x10, k.label) || !ue::mread(at, k.orig, sizeof k.orig) || !tp_sane(k.orig, 6)) continue;
                if (k.vt < gb || k.vt >= ge) continue;
                if (g_tp_cal_by_vt ? (k.vt != g_tp_vt || k.label != g_tp_label) : (k.orig[0] != kTpOrig[0] || k.orig[1] != kTpOrig[1] || k.orig[2] != kTpOrig[2])) continue;
                k.y = 0.125 * (double)(g_tp_cal.size() + 1);
                g_tp_cal.push_back(k);
            }
            if (g_tp_cal.empty()) { tp_cal_give_up("third-person camera data not found"); return; }
            for (const TpCand& k : g_tp_cal) tp_write(k.at, k.orig[0], k.y, k.orig[2]);
            cam_set_mode("ThirdPerson", 0.0, true); g_tp_cal_step = 1; g_tp_cal_at = now; return;
        }
        if (now - g_tp_cal_at < 300) return;
        double o[3] = {}; const bool got = tp_state_offset(o);
        int hit = -1; for (size_t i = 0; got && i < g_tp_cal.size(); ++i) if (std::fabs(o[1] - g_tp_cal[i].y) < 1e-6) hit = (int)i;
        for (const TpCand& k : g_tp_cal) {   // put the original Y back - only where the object and our test value are still there
            uint64_t vt = 0, lb = 0; double cur[3];
            if (ue::rd_as(k.at - 0x18, vt) && ue::rd_as(k.at - 0x10, lb) && vt == k.vt && lb == k.label && ue::mread(k.at, cur, sizeof cur) &&
                cur[0] == k.orig[0] && cur[1] == k.y && cur[2] == k.orig[2]) tp_write(k.at, k.orig[0], k.orig[1], k.orig[2]);
        }
        g_tp_cal_step = 0;
        if (hit < 0) {   // a real attempt counter (the old time check could never fire: the camera was restarted 3x a second for ever)
            if (++g_tp_cal_tries >= 5) { cam_set_mode("ThirdPerson", 0.3, true); tp_cal_give_up("could not identify the third-person camera"); }
            else g_tp_cal_next = now + 1000;
            return;
        }
        const TpCand k = g_tp_cal[hit]; g_tp_cal.clear(); g_tp_cal_tries = 0;
        g_tp_def = k.at; g_tp_vt = (uintptr_t)k.vt; g_tp_label = (uintptr_t)k.label;
        if (!g_tp_cal_by_vt) memcpy(g_tp_orig, k.orig, sizeof g_tp_orig);   // pattern scan = untouched values; by vtable the cached originals stay
        g_tp_scan = 3; g_tp_msg = "ready"; tp_cache_save(); plog("tp: ThirdPerson offset at " + hexstr(g_tp_def));
        tp_apply(0.3); return;
    }
    if (!tp_valid()) { g_tp_scan = 0; g_tp_def = 0; tp_idle(); return; }
    {   const uintptr_t pw = player_pawn();
        if (!pw) { tp_idle(); return; }                       // loading / dead: nothing to aim with
        if (pw != g_tp_pawn) g_tp_applied = false; }          // new character (map change, respawn): the game put it in its own mode
    if (!g_tp_applied) { if (now - g_tp_applied_at > 500) { tp_apply(0.3); g_tp_iter = 0; } return; }
    {   const int mi = g_tp_aiming && g_cam.value("tp_aim", true) ? 1 : 0;
        TpMeas& m = g_tp_meas[mi];
        if (GetTickCount64() - g_tp_applied_at > 700 && g_tp_iter < 3 && (!m.ok || m.key != tp_key(mi == 1))) {
            TpMeas n; bool got = false; try { got = tp_measure(n); } catch (...) { got = false; }
            g_cam["tp_meas"] = { {"got", got}, {"lat", n.lat}, {"vert", n.vert}, {"back", n.back}, {"iter", g_tp_iter} };
            if (!got) ++g_tp_iter;
            if (got) { n.key = tp_key(mi == 1); const bool changed = !m.ok || std::fabs(n.lat - m.lat) > 3 || std::fabs(n.vert - m.vert) > 3 || std::fabs(n.back - m.back) > 6;
                m = n; ++g_tp_iter; if (changed) tp_apply(0.15); } }
    }
    // aim zoom: the game's own zoom state on the pawn (OakCharacter.ZoomState.State != 0 while aiming) - works for mouse,
    // gamepad and the game's "toggle zoom" option alike
    bool zoom = false;
    if (const uintptr_t pawn = player_pawn()) {   // field offset looked up once per pawn class
        static uintptr_t cls = 0; static size_t off = 0; uint8_t zs = 0;
        if (ue::class_of(pawn) != cls) { uintptr_t b = 0; ue::Prop leaf; cls = ue::class_of(pawn); off = ue::resolve(pawn, cls, "ZoomState.State", b, leaf) ? (b - pawn) + leaf.offset : 0; }
        if (off && ue::rd_as(pawn + off, zs)) zoom = zs != 0; }
    const bool aim = g_cam.value("tp_aim", true) && zoom;
    if (aim != g_tp_aiming) { g_tp_aiming = aim; g_tp_iter = 0; tp_apply(aim ? 0.16 : 0.22); }
    g_tp_xhair = aim;
    // shoulder swap while aiming: L3 (left stick click - hidden from the game meanwhile so it does not sprint out of the
    // aim) or the middle mouse button. The side is remembered (camera.json) for hip-fire and later sessions.
    {
        PadShare* ps = pad_share(); static bool l3_was = false, mmb_was = false;
        if (ps) ps->mask = aim ? XINPUT_GAMEPAD_LEFT_THUMB : 0;
        bool l3 = ps && (ps->raw & XINPUT_GAMEPAD_LEFT_THUMB);
        // while aiming (the swap only works then) and only the slot that answered - XInputGetState on an empty slot costs
        // milliseconds, and all four were polled every tick. Empty slots are probed again every 3 s.
        const bool host_feeds = ps && (ps->packet != 0 || ps->raw != 0);
        if (aim && !l3 && !host_feeds) {
            static int slot = -1; static ULONGLONG probe_at = 0;
            if (slot >= 0) { XINPUT_STATE xs{}; if (XInputGetState((DWORD)slot, &xs) == ERROR_SUCCESS) l3 = (xs.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0; else slot = -1; }
            else if (now - probe_at > 3000) { probe_at = now;
                for (DWORD pi = 0; pi < 4; ++pi) { XINPUT_STATE xs{};
                    if (XInputGetState(pi, &xs) == ERROR_SUCCESS) { slot = (int)pi; l3 = (xs.Gamepad.wButtons & XINPUT_GAMEPAD_LEFT_THUMB) != 0; break; } } }
        }
        HWND fg = GetForegroundWindow(); DWORD fpid = 0; GetWindowThreadProcessId(fg, &fpid);
        const bool mmb = fpid == GetCurrentProcessId() && !wb::overlay_visible() && (GetAsyncKeyState(VK_MBUTTON) & 0x8000);
        { static bool aim_was = false; if (aim && !aim_was) { l3_was = l3; mmb_was = mmb; } aim_was = aim; }   // a button already held when the aim starts is not a press
        if (aim && ((l3 && !l3_was) || (mmb && !mmb_was))) {
            g_cam["tp_left"] = !g_cam.value("tp_left", false); tp_apply(0.22); tp_settings_save();
            plog(std::string("tp: shoulder -> ") + (g_cam.value("tp_left", false) ? "left" : "right"));
            cam_snap();   // the panel shows the new side
        }
        l3_was = l3; mmb_was = mmb;
    }
    const double dt = std::min(0.1, (now - g_tp_tick_at) / 1000.0); g_tp_tick_at = now;
    // base = the trainer's FOV slider, else the game's own FOV (sampled while we are not writing it; it is not always 90)
    static double base_fov = 0; static ULONGLONG base_at = 0;
    if (fov_out <= 0 && !aim && g_tp_fov <= 0 && now - base_at > 500) { base_at = now;
        const uintptr_t cm = cam_mgr(); const double f = cm ? prop_n(cm, "CameraCachePrivate.POV.FOV") : 0; if (f >= 30 && f <= 170) base_fov = f; }
    const double base = fov_out > 0 ? fov_out : (base_fov > 0 ? base_fov : 90.0), target = aim ? g_cam.value("tp_aim_fov", 60.0) : base;
    if (g_tp_fov <= 0) { if (!aim) return; g_tp_fov = base; }
    g_tp_fov += (target - g_tp_fov) * std::min(1.0, dt * 12.0);
    if (!aim && std::fabs(g_tp_fov - base) < 0.3) { g_tp_fov = 0; return; }
    fov_out = g_tp_fov;
}

static void camera_tick() {
    tp_settings_load();
    if (!g_cam_saved.is_null() && player_pawn()) cam_recover();
    if (g_cam_restore_pending && (player_pawn() || player_pc())) cam_restore_all("world is back");    // a restore that had to wait
    matrix_tick();
    const ULONGLONG now = GetTickCount64();
    const double fov = g_cam.value("fov", 0.0);
    if (g_cam.value("free", false)) {
        tp_idle();
        {   const uintptr_t pw = player_pawn();   // camera destroyed / player died or respawned: unfreeze the player, view back on it
            if (!g_cam_actor || !ue::alive(g_cam_actor) || !pw || pw != g_cam_pawn) { cam_restore_all("the free camera lost its camera or its player"); return; } }
        const double dt = std::min(0.1, (now - g_cam_at) / 1000.0); g_cam_at = now;
        HWND fg = GetForegroundWindow(); DWORD pid = 0; GetWindowThreadProcessId(fg, &pid);
        json cr = ue::call_json(player_pc(), "GetControlRotation", json::object(), 0);
        double pitch = 0, yaw = 0;
        if (cr.value("ok", false)) { const json& v = cr["data"]["ReturnValue"]["value"]; pitch = rot_get(v, "Pitch"); yaw = rot_get(v, "Yaw"); }
        if (pitch > 180) pitch -= 360;
        if (pid == GetCurrentProcessId()) {
            auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
            const double pr = pitch * 3.14159265 / 180, yr = yaw * 3.14159265 / 180;
            const V3 fwd{ std::cos(pr) * std::cos(yr), std::cos(pr) * std::sin(yr), std::sin(pr) }, right{ -std::sin(yr), std::cos(yr), 0 };
            double mv = 1200.0 * g_cam.value("speed", 1.0) * dt * (held(VK_SHIFT) ? 4.0 : 1.0);
            V3 d{};
            if (held('W')) { d.x += fwd.x; d.y += fwd.y; d.z += fwd.z; }
            if (held('S')) { d.x -= fwd.x; d.y -= fwd.y; d.z -= fwd.z; }
            if (held('D')) { d.x += right.x; d.y += right.y; }
            if (held('A')) { d.x -= right.x; d.y -= right.y; }
            if (held(VK_SPACE)) d.z += 1;
            if (held('C')) d.z -= 1;
            g_cam_pos.x += d.x * mv; g_cam_pos.y += d.y * mv; g_cam_pos.z += d.z * mv;
        }
        ue::call_json(g_cam_actor, "K2_SetActorLocationAndRotation", { {"NewLocation", { {"X", g_cam_pos.x}, {"Y", g_cam_pos.y}, {"Z", g_cam_pos.z} }},
            {"NewRotation", { {"Pitch", pitch}, {"Yaw", yaw}, {"Roll", 0.0} }}, {"bSweep", false}, {"bTeleport", true} }, 0);
        return;
    }
    double fov_w = fov;
    if (g_cam.value("mode", std::string()) == "ThirdPerson") tp_tick(fov_w);
    else { if (g_tp_applied) tp_restore(); tp_idle(); }
    if (fov_w > 0) { const double fov = fov_w;   // player camera FOV: written after the game every tick
        const uintptr_t cm = cam_mgr(); if (!cm) return;
        uintptr_t b = 0; ue::Prop leaf;
        if (ue::resolve(cm, ue::class_of(cm), "CameraCachePrivate.POV.FOV", b, leaf)) ue::set_num(b, leaf, fov);
    }
}
// leaving third person: the definition gets its original values back - also when THIS copy never applied them (after a hot
static void tp_leave() {
    if (g_tp_cal_step == 1) {
        for (const TpCand& k : g_tp_cal) {
            uint64_t vt = 0, lb = 0; double cur[3];
            if (ue::rd_as(k.at - 0x18, vt) && ue::rd_as(k.at - 0x10, lb) && vt == k.vt && lb == k.label && ue::mread(k.at, cur, sizeof cur) &&
                cur[0] == k.orig[0] && cur[1] == k.y && cur[2] == k.orig[2]) tp_write(k.at, k.orig[0], k.orig[1], k.orig[2]);
        }
        g_tp_cal_step = 0; g_tp_cal.clear();
    }
    if (g_tp_scan.load() == 0) tp_load_cache();
    tp_restore();
}
// entering third person: applied by tp_tick; a search that failed earlier gets a new try
static void tp_enter() { g_tp_applied = false; if (g_tp_scan.load() == 4) { g_tp_scan = 0; g_tp_cal_tries = 0; g_tp_cal_next = 0; } }
static json camera_cmd(const json& a) {
    tp_settings_load();
    json s0 = session(); if (!s0.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    if (!g_cam_saved.is_null() && player_pawn()) cam_recover();
    if (a.contains("third")) {   // third person on / off / "toggle"
        const bool on = a["third"].is_string() ? g_cam.value("mode", std::string()) != "ThirdPerson" : a["third"].get<bool>();
        const std::string m = on ? "ThirdPerson" : "Default";
        if (!on) tp_leave();
        if (cam_set_mode(m)) g_cam["mode"] = m;
        if (on) tp_enter(); }
    if (a.contains("mode")) { const std::string m = a["mode"].get<std::string>(); if (m != "ThirdPerson") tp_leave(); else tp_enter(); if (!cam_set_mode(m)) return fail("MODE", "camera mode " + m + " refused"); g_cam["mode"] = m; }
    if (a.contains("hud")) { const bool on = a["hud"].is_string() ? !(g_cam.value("hud", true)) : a["hud"].get<bool>(); if (cam_hud(on)) g_cam["hud"] = on; }
    if (a.contains("fov")) { const double f = a["fov"].get<double>(); g_cam["fov"] = f <= 0 ? 0.0 : std::clamp(f, 30.0, 170.0); cam_lens(g_cam.value("fov", 0.0)); }
    if (a.contains("speed")) g_cam["speed"] = std::clamp(a["speed"].get<double>(), 0.05, 20.0);
    if (a.contains("freeze")) cam_freeze(a["freeze"].is_string() ? !g_cam.value("freeze", false) : a["freeze"].get<bool>());
    for (const char* k : { "matrix_slow", "rain_density", "rain_speed", "rain_alpha", "rain_size" }) if (a.contains(k) && a[k].is_number()) g_cam[k] = a[k].get<double>();
    {   // native third person settings
        bool tp_changed = false;
        for (const char* k : { "tp_dist", "tp_side", "tp_height", "tp_aim_dist", "tp_aim_side", "tp_aim_fov", "tp_converge" }) if (a.contains(k) && a[k].is_number()) { g_cam[k] = a[k].get<double>(); tp_changed = true; }
        for (const char* k : { "tp", "tp_left", "tp_aim" }) if (a.contains(k)) { const bool cur = g_cam.value(k, false);
            g_cam[k] = a[k].is_boolean() ? a[k].get<bool>() : a[k].is_number() ? a[k].get<double>() != 0 : !cur; tp_changed = true; }
        if (a.contains("tp") && g_cam.value("tp", true)) tp_enter();
        if (tp_changed) { tp_settings_save(); g_tp_meas[0].ok = g_tp_meas[1].ok = false; g_tp_iter = 0; }
        if (tp_changed && g_cam.value("mode", std::string()) == "ThirdPerson" && g_cam.value("tp", true)) tp_apply(0.3);
    }
    if (a.contains("rain")) g_cam["rain"] = a["rain"].is_string() ? !g_cam.value("rain", true) : a["rain"].get<bool>();
    if (a.contains("matrix_tint")) { g_cam["matrix_tint"] = a["matrix_tint"].is_string() ? !g_cam.value("matrix_tint", true) : a["matrix_tint"].get<bool>(); matrix_tint(g_cam.value("matrix_tint", true) && g_cam.value("matrix", false)); }
    if (a.contains("matrix_slow") && g_cam.value("bullet", false)) bullet_time(true);   // live change of the bullet-time speed
    if (a.contains("bullet")) { const bool on = a["bullet"].is_string() ? !g_cam.value("bullet", false) : a["bullet"].get<bool>();
        if (!player_pawn()) return fail("NO_PAWN", "load into the game first"); bullet_time(on); plog(on ? "camera: bullet time on" : "camera: bullet time off"); }
    if (a.contains("matrix")) { const bool on = a["matrix"].is_string() ? !g_cam.value("matrix", false) : a["matrix"].get<bool>(); json r = cam_matrix(on); if (!r.value("ok", false)) return r; }
    if (a.contains("free")) { const bool on = a["free"].is_string() ? !g_cam.value("free", false) : a["free"].get<bool>(); json r = cam_free(on); if (!r.value("ok", false)) return r; }
    if (a.value("reset", false)) {   // whatever the flags say: definition restored, world / player time 1, view on the player, our camera gone
        tp_leave(); cam_restore_all("reset"); matrix_tint(false); cam_set_mode("Default"); cam_hud(true);
        g_cam = { {"mode", "Default"}, {"hud", true}, {"free", false}, {"fov", 0.0}, {"speed", 1.0}, {"freeze", false},
                  {"matrix", false}, {"bullet", false}, {"matrix_slow", 0.2}, {"matrix_tint", true},
                  {"rain", true}, {"rain_density", 1.3}, {"rain_speed", 1.0}, {"rain_alpha", 0.9}, {"rain_size", 24.0},
    {"tp", true}, {"tp_dist", 230.0}, {"tp_side", 65.0}, {"tp_height", -15.0}, {"tp_left", false}, {"tp_aim", true}, {"tp_aim_dist", 120.0}, {"tp_aim_side", 50.0}, {"tp_aim_fov", 60.0} }; }
    rain_sync(); g_cam["tp_status"] = g_tp_msg;
    if (PadShare* ps = pad_share()) g_cam["pad"] = { {"raw", (int)ps->raw}, {"packet", (int)ps->packet}, {"mask", (int)ps->mask} };
    cam_snap();
    try { tp_settings_save(); } catch (...) {}
    return ok(g_cam);
}

// ------------------------------------------------------------------ time of day (game's TimeOfDayBlueprintLibrary, 0..1 = day cycle)
static json time_cmd(const json& a) {
    uintptr_t pawn = player_pawn(); if (!pawn) return fail("NO_PAWN", "no player character");
    const uintptr_t lib = ue::cdo_of(ue::find_class("TimeOfDayBlueprintLibrary"));
    const json ctx = { {"WorldContextObject", "addr:" + hexstr(pawn)} };
    if (a.contains("v") || a.contains("hour")) {
        json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
        double v = a.contains("hour") ? a["hour"].get<double>() / 24.0 : a["v"].get<double>();
        if (v > 1.0) v /= 24.0;   // hours given as v
        json c = ctx; c["NewTimeOfDay"] = std::clamp(v, 0.0, 0.9999);
        json r = ue::call_json(lib, "SetTimeOfDay", c, 0); if (!r.value("ok", false)) return r;
    }
    if (a.contains("pause")) {   // a write like the others: solo only
        json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
        const bool p = a["pause"].is_boolean() ? a["pause"].get<bool>() : a["pause"].is_number() && a["pause"].get<double>() != 0;
        ue::call_json(lib, p ? "PauseTimeOfDay" : "StartTimeOfDay", ctx, 0);
    }
    json g = ue::call_json(lib, "GetTimeOfDay", ctx, 0);
    const double t = g.value("ok", false) ? g["data"]["ReturnValue"].value("value", 0.0) : -1;
    return ok({ {"time", t}, {"hour", std::round(t * 240) / 10.0} });
}

// ------------------------------------------------------------------ teleport to the map waypoint (OakPlayerState.DiscoveryPinningState)
// PinnedDatas[] = GbxDiscoveryPinningPinData (0x78): PinnedCustomWaypointLocation Vector @0x18. The custom map pin the
// player sets on the map (M). Z of a map pin may be 0 -> drop in from above (z_offset) with god mode / or ground trace later.
static json teleport_to(double x, double y, double z);
// ground height under (x,y): engine KismetSystemLibrary::LineTraceSingle straight down (Visibility channel)
static bool ground_z(double x, double y, double from_z, double& out_z) {
    const uintptr_t ksl = ue::cdo_of(ue::find_class("KismetSystemLibrary"));
    ue::Params p = ue::params_for(ksl, "LineTraceSingle"); if (!p) return false;
    const uintptr_t pawn = player_pawn();
    auto putv = [&](const char* n, double a, double b, double c) { ue::Prop pp = p.param(n); if (!pp) return; double v[3] = { a, b, c }; memcpy(p.at(pp), v, 24); };
    { ue::Prop w = p.param("WorldContextObject"); if (w) memcpy(p.at(w), &pawn, 8); }
    putv("Start", x, y, from_z); putv("End", x, y, from_z - 200000.0);
    p.put_bool("bIgnoreSelf", true);
    std::string e; if (!ue::call(ksl, p, &e) || !p.ret<bool>()) return false;
    ue::Prop hit = p.param("OutHit"); if (!hit) return false;
    ue::Prop ip = ue::find_prop(hit.owner_struct, "ImpactPoint"); if (!ip) return false;
    double v[3] = {}; memcpy(v, p.at(hit) + ip.offset, 24); out_z = v[2]; return true;
}
// line trace from a to b (ignores the player); true + impact point when something is in between
static bool line_hit(const V3& a, const V3& b, V3& hitp) {
    static uintptr_t ksl = 0; if (!ksl || !ue::alive(ksl)) ksl = ue::cdo_of(ue::find_class("KismetSystemLibrary"));
    ue::Params p = ue::params_for(ksl, "LineTraceSingle"); if (!p) return false;
    const uintptr_t pawn = player_pawn();
    auto putv = [&](const char* n, const V3& v) { ue::Prop pp = p.param(n); if (!pp) return; double d[3] = { v.x, v.y, v.z }; memcpy(p.at(pp), d, 24); };
    { ue::Prop w = p.param("WorldContextObject"); if (w) memcpy(p.at(w), &pawn, 8); }
    putv("Start", a); putv("End", b); p.put_bool("bIgnoreSelf", true);
    std::string e; if (!ue::call(ksl, p, &e) || !p.ret<bool>()) return false;
    ue::Prop hit = p.param("OutHit"); if (!hit) return false;
    ue::Prop ip = ue::find_prop(hit.owner_struct, "ImpactPoint"); if (!ip) return false;
    double v[3] = {}; memcpy(v, p.at(hit) + ip.offset, 24); hitp = { v[0], v[1], v[2] }; return true;
}
static json waypoint_cmd(const json& a) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    uintptr_t ps = player_ps(); if (!ps) return fail("NO_PS", "no player state");
    uintptr_t base = 0; ue::Prop leaf;
    if (!ue::resolve(ps, ue::class_of(ps), "DiscoveryPinningState.PinnedDatas", base, leaf)) return fail("LAYOUT", "DiscoveryPinningState.PinnedDatas not found");
    auto arr = ue::array_of(base, leaf);
    json pins = json::array(); V3 target; bool have = false;
    for (int32_t i = 0; i < arr.num && i < 32; ++i) {
        const uintptr_t e = (uintptr_t)arr.data + (uintptr_t)i * 0x78;
        double v[3] = {}; ue::mread(e + 0x18, v, sizeof v); uint8_t type = 0; ue::rd_as(e + 8, type);
        pins.push_back({ {"type", type}, {"X", v[0]}, {"Y", v[1]}, {"Z", v[2]} });
        if (std::fabs(v[0]) + std::fabs(v[1]) > 1) { target = { v[0], v[1], v[2] }; have = true; }
    }
    if (a.value("list", false)) return ok({ {"pins", pins} });
    if (!have) return fail("NO_PIN", "no map pin set - open the map (M) and right-click a spot (Custom Pin)");
    // map pins carry a placeholder height (500) -> find the ground below with a trace from high above
    double gz = 0; const bool hit = ground_z(target.x, target.y, a.value("trace_from", 60000.0), gz);
    const double z = hit ? gz + 150.0 : a.value("z", 30000.0);
    json r = teleport_to(target.x, target.y, z);
    if (r.value("ok", false)) { r["data"]["pin"] = { {"X", target.x}, {"Y", target.y} }; r["data"]["ground"] = hit ? json(gz) : json("not found (dropped from high)"); }
    return r;
}


// ------------------------------------------------------------------ mission markers (read-only + teleport; never writes mission data)
// GbxDiscoveryProviderObject_OakMissionWaypoint: +0xC8 -> location object (position doubles @+0x50), +0xE0 -> display def
// (category FName @+0x18: DiscoveryCategory_Main / _Secondary ...). Missions are completed by the game itself.
static std::string def_name_at(uintptr_t def, int off);
static json missions_cmd(const json& a) {
    const uintptr_t pawn = player_pawn(); V3 me; if (!pawn || !actor_loc(pawn, me)) return fail("NO_PAWN", "load into the game first");
    json marks = json::array(); std::vector<V3> pos;
    for (uintptr_t w : ue::find_all(ue::find_class("GbxDiscoveryProviderObject_OakMissionWaypoint"), false, 200)) {
        uintptr_t loc = 0, def = 0; ue::rd_as(w + 0xC8, loc); ue::rd_as(w + 0xE0, def);
        double v[3] = {}; if (!loc || !ue::mread(loc + 0x50, v, sizeof v)) continue;
        if (std::fabs(v[0]) + std::fabs(v[1]) < 1 || !std::isfinite(v[0]) || std::fabs(v[0]) > 5e7) continue;
        std::string cat = def ? def_name_at(def, 0x18) : ""; if (cat.rfind("DiscoveryCategory_", 0) == 0) cat = cat.substr(18);
        const double d = std::sqrt((v[0] - me.x) * (v[0] - me.x) + (v[1] - me.y) * (v[1] - me.y) + (v[2] - me.z) * (v[2] - me.z)) / 100.0;
        marks.push_back({ {"i", (int)pos.size()}, {"category", cat.empty() ? "?" : cat}, {"dist_m", std::round(d)}, {"X", v[0]}, {"Y", v[1]}, {"Z", v[2]} });
        pos.push_back({ v[0], v[1], v[2] });
    }
    if (a.contains("go") || a.contains("v")) {
        json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
        int i = a.contains("go") ? a["go"].get<int>() : -1;
        if (i < 0) {   // "nearest" / category filter
            const std::string want = a.contains("v") && a["v"].is_string() ? a["v"].get<std::string>() : "";
            double best = 1e18;
            for (auto& m : marks) { if (!want.empty() && want != "nearest" && m["category"] != want) continue;
                if (m["dist_m"].get<double>() > 3 && m["dist_m"].get<double>() < best) { best = m["dist_m"].get<double>(); i = m["i"].get<int>(); } }
        }
        if (i < 0 || i >= (int)pos.size()) return fail("NO_MARKER", "no mission marker to go to");
        double gz = 0; bool hit = ground_z(pos[i].x, pos[i].y, pos[i].z + 250.0, gz);   // just above the marker: land at its level, not on a roof
        if (!hit) hit = ground_z(pos[i].x, pos[i].y, pos[i].z + 3000.0, gz);
        json r = teleport_to(pos[i].x, pos[i].y, hit ? gz + 150.0 : pos[i].z + 200.0);
        if (r.value("ok", false)) r["message"] = "teleported to " + marks[i]["category"].get<std::string>() + " marker (" + std::to_string((int)marks[i]["dist_m"].get<double>()) + " m)";
        return r;
    }
    return ok({ {"markers", marks} }, std::to_string(marks.size()) + " mission marker(s)");
}

// ------------------------------------------------------------------ HUD: enemy info + loot radar (overlay, always on when enabled)
// Game thread (every 150 ms): hostiles + pickups within range -> screen position with the engine's
// PlayerController::ProjectWorldLocationToScreen, enemy level / health % from Gearbox attributes. Render thread draws the
// last snapshot (wb::hud_draw). Settings: BL.HUD {on, enemies, loot, radius_m}; saved in <data>\hud.json.
struct HudMark { float x, y; std::string text; uint32_t color; };
static json pickup_info(uintptr_t pk);
static std::mutex g_hud_mtx;
static std::vector<HudMark> g_hud_marks;
static ULONGLONG g_hud_at = 0;
static json g_hud_cfg = { {"on", false}, {"enemies", true}, {"loot", true}, {"radius_m", 80.0} };
static bool project(uintptr_t pc, const V3& w, float& sx, float& sy) {
    ue::Params p = ue::params_for(pc, "ProjectWorldLocationToScreen"); if (!p) return false;
    ue::Prop wl = p.param("WorldLocation"); if (!wl) return false;
    double v[3] = { w.x, w.y, w.z }; memcpy(p.at(wl), v, 24);
    std::string e; if (!ue::call(pc, p, &e) || !p.ret<bool>()) return false;
    ue::Prop sl = p.param("ScreenLocation"); if (!sl) return false;
    double s[2] = {}; memcpy(s, p.at(sl), 16); sx = (float)s[0]; sy = (float)s[1]; return true;
}
static void hud_tick() {
    static ULONGLONG last = 0; const ULONGLONG now = GetTickCount64();
    if (!g_hud_cfg.value("on", false) || now - last < 150) return; last = now;
    uintptr_t pawn = player_pawn(), pc = player_pc(); V3 me; if (!pawn || !pc || !actor_loc(pawn, me)) return;
    const double r = g_hud_cfg.value("radius_m", 80.0) * 100.0;
    std::vector<HudMark> marks;
    auto dist = [&](const V3& v) { return std::sqrt((v.x - me.x) * (v.x - me.x) + (v.y - me.y) * (v.y - me.y) + (v.z - me.z) * (v.z - me.z)); };
    // sliced object lists + raw location reads + remembered teams: no full object walk (it was two every 150 ms) and no
    // game call for anything out of range
    if (g_hud_cfg.value("enemies", true)) {
        g_char_scan.want();
        for (uintptr_t c : g_char_scan.live()) {
            if (c == pawn) continue; V3 v; if (!actor_loc_fast(c, v)) continue; const double d = dist(v); if (d > r) continue;
            const std::string t = team_cached(c); if (friendly_team(t)) continue;
            // enemies use health POOLS (the player's health_resource_* attributes read 0 on them):
            // DamageStatics::GetHealthPoolPercent(layer 0) = health, alive check = OakCharacter::IsHealthy
            { json h = ue::call_json(c, "IsHealthy", json::object(), 0); if (h.value("ok", false) && h["data"]["ReturnValue"].value("value", 1.0) == 0) continue; }
            static uintptr_t ds = 0; if (!ds || !ue::alive(ds)) ds = ue::cdo_of(ue::find_class("DamageStatics"));
            json hr = ue::call_json(ds, "GetHealthPoolPercent", { {"Context", "addr:" + hexstr(c)}, {"layer", 0} }, 0);
            json hp = hr.value("ok", false) ? hr["data"]["ReturnValue"]["value"] : json(); json lv = attr_get("Att_ActorExperienceLevel", c);
            v.z += 120; float sx, sy; if (!project(pc, v, sx, sy)) continue;
            std::string nm = ue::name_of(c); if (nm.rfind("Char_", 0) == 0) nm = nm.substr(5);
            { const size_t u = nm.find("_21474"); if (u != std::string::npos) nm.resize(u); }
            char b[160]; snprintf(b, sizeof b, "%s  Lv %d  %d%%  %dm", nm.c_str(), lv.is_number() ? (int)lv.get<double>() : 0, hp.is_number() ? (int)std::lround(hp.get<double>() * 100) : -1, (int)(d / 100));
            marks.push_back({ sx, sy, b, 0xFF5050FF });
        }
    }
    if (g_hud_cfg.value("loot", true)) {
        g_pick_scan.want();
        for (uintptr_t x : g_pick_scan.live()) {
            V3 v; if (!actor_loc_fast(x, v)) continue; const double d = dist(v); if (d > r) continue;
            v.z += 40; float sx, sy; if (!project(pc, v, sx, sy)) continue;
            static const char* rn[] = { "", "common ", "uncommon ", "rare ", "epic ", "legendary ", "pearl " };
            static const uint32_t rc[] = { 0xFF40D0FF, 0xFFE0E0E0, 0xFF40E040, 0xFFFF9040, 0xFFFF50C0, 0xFF20A0FF, 0xFFFFFFA0 };   // ABGR
            const int r = std::clamp(pickup_info(x).value("rarity", 0), 0, 6);
            char b[80]; snprintf(b, sizeof b, "LOOT %s%dm", rn[r], (int)(d / 100));
            marks.push_back({ sx, sy, b, rc[r] });
        }
    }
    std::lock_guard<std::mutex> lk(g_hud_mtx); g_hud_marks.swap(marks); g_hud_at = now;
}
// one HUD setting with the right type only (a text "on" used to be stored, made every hud_tick throw and was saved to hud.json)
static void hud_set(const char* k, const json& v) {
    if (!strcmp(k, "radius_m")) { if (v.is_number()) g_hud_cfg[k] = std::clamp(v.get<double>(), 5.0, 2000.0); return; }
    const bool cur = g_hud_cfg[k].is_boolean() ? g_hud_cfg[k].get<bool>() : false;
    if (v.is_boolean()) g_hud_cfg[k] = v.get<bool>(); else if (v.is_number()) g_hud_cfg[k] = v.get<double>() != 0; else if (v.is_string()) g_hud_cfg[k] = !cur;   // text = "toggle"
}
static json hud_cmd(const json& a) {
    for (const char* k : { "on", "enemies", "loot", "radius_m" }) if (a.contains(k)) hud_set(k, a[k]);
    save_toggles(); std::ofstream(g_data_dir + "\\hud.json") << g_hud_cfg.dump(1);
    if (!g_hud_cfg.value("on", false)) { std::lock_guard<std::mutex> lk(g_hud_mtx); g_hud_marks.clear(); }
    return ok(g_hud_cfg);
}

// ------------------------------------------------------------------ weapon builder (light): maker + type + rarity + level
// Parts cannot be chosen yet (the generator that takes selection criteria is native, see HANDOFF). The builder picks the
// game's own pool for maker+type ("itempool_<m>_<t>", exact), a rarity pool per type, or a named legendary pool, spawns
// with SpawnInventoryFromItemPool at the chosen level and - when a rarity is wanted - READS the rarity of the new
// pickup (identity +8 = InventoryCompDef, FName @+0x48 = "..comp_0N_<rarity>[_Name]") and re-rolls: wrong pickups are
// removed with Actor::K2_DestroyActor. Gear luck (Att_Luck_Gear) is raised during the rolls for epic and better.
static json g_builder_cat, g_builder = { {"maker", ""}, {"type", "ar"}, {"rarity", 0}, {"level", 0}, {"legendary", ""} };   // = the panel's first options
static const json& builder_cat() {
    if (g_builder_cat.is_null()) { const size_t k = g_api_path.find_last_of("\\/"); g_builder_cat = read_json(g_api_path.substr(0, k) + "\\builder.json"); if (!g_builder_cat.is_object()) g_builder_cat = json::object(); }
    return g_builder_cat;
}
static uintptr_t pickup_identity(uintptr_t pk) {
    static int item_off = -1;
    if (item_off < 0) { uintptr_t b = 0; ue::Prop leaf; if (ue::resolve(pk, ue::class_of(pk), "Item", b, leaf)) item_off = (int)(b - pk) + leaf.offset; else return 0; }
    return pk + item_off + 8;   // GbxItem.data (GbxItemData) +8 = InventoryIdentity
}
static std::string def_name_at(uintptr_t def, int off) { if (!def) return ""; ue::FName n{}; if (!ue::rd_as(def + off, n)) return ""; return ue::name_str(n); }
static json pickup_info(uintptr_t pk) {   // {type, comp, rarity 1..6 (0 unknown)}
    const uintptr_t id = pickup_identity(pk); if (!id) return json::object();
    uintptr_t t = 0, c = 0; ue::rd_as(id, t); ue::rd_as(id + 8, c);
    const std::string tn = def_name_at(t, 0x58), cn = def_name_at(c, 0x48);
    int r = 0; const size_t k = cn.find("comp_0"); if (k != std::string::npos && k + 6 < cn.size() && isdigit((unsigned char)cn[k + 6])) r = cn[k + 6] - '0';
    if (cn.find("pearl") != std::string::npos) r = 6;
    return { {"type", tn}, {"comp", cn}, {"rarity", r} };
}
// The pickup list comes from the shared sliced scan g_pick_scan (SliceScan, top of the file; stepped by the keeper).
// `before` = pickups that are not this roll's drop, address -> the item's InstanceId: an address reused by a new pickup
// (the wrong rolls are destroyed, the game reuses the memory) has another id and counts as new.
struct BuildJob {
    bool active = false; std::string pool; int want = 0, level = 50, tries = 0, max_tries = 40, kept = 0;
    bool need_base = false; uint64_t base_pass = 0, spawn_pass = 0, seen_pass = 0; int empty = 0, asked_level = 0;
    std::map<uintptr_t, uint32_t> before; ULONGLONG spawned_at = 0, seen_at = 0; std::vector<uintptr_t> fresh;
    std::vector<uint8_t> luck_handle; json result; V3 at;   // at = where the roll was spawned: only pickups near it are the roll
};
static BuildJob g_build;
static uint32_t pickup_iid(uintptr_t pk) { const uintptr_t id = pickup_identity(pk); uint32_t v = 0; if (id) ue::rd_as(id - 8, v); return v; }   // GbxItemData.InstanceId
static bool build_known(uintptr_t x) { auto it = g_build.before.find(x); return it != g_build.before.end() && it->second == pickup_iid(x); }
static std::string luck_file() { return g_data_dir + "\\build_luck.json"; }
static AttrMod g_luck_orphan;
static void build_finish(json r);
static void build_spawn_now();
// first roll: wait for a full sliced pass to know which pickups already lie around; re-rolls: the old drop joins `before`
static void build_spawn() {
    if (g_build.tries == 0) { g_build.need_base = true; g_build.base_pass = g_pick_scan.passes; g_build.before.clear(); g_build.fresh.clear(); return; }
    for (uintptr_t x : g_build.fresh) g_build.before[x] = pickup_iid(x);
    build_spawn_now();
}
static void build_spawn_now() {
    uintptr_t pawn = player_pawn(); V3 p, f; if (!pawn || !actor_loc(pawn, p) || !actor_fwd(pawn, f)) { build_finish(fail("NO_PAWN", "no player")); return; }   // through build_finish: the gear luck comes off
    g_build.fresh.clear(); g_build.seen_at = 0; g_build.spawn_pass = g_pick_scan.passes;
    g_build.at = { p.x + f.x * 250, p.y + f.y * 250, p.z + 60 };
    json tr = { {"Rotation", { {"W", 1.0} }}, {"Translation", { {"X", p.x + f.x * 250}, {"Y", p.y + f.y * 250}, {"Z", p.z + 60} }}, {"Scale3D", { {"X", 1.0}, {"Y", 1.0}, {"Z", 1.0} }} };
    ue::call_json(ue::cdo_of(ue::find_class("NexusConfigStoreItemPool")), "SpawnInventoryFromItemPool",
        { {"WorldContextObject", "addr:" + hexstr(pawn)}, {"Transform", tr}, {"GameStage", g_build.level}, {"ItemPoolName", "name:" + g_build.pool} }, pawn);
    g_build.spawned_at = GetTickCount64(); ++g_build.tries;
}
static void build_finish(json r) {
    if (!g_build.luck_handle.empty()) {   // undo the temporary gear luck
        AttrMod tmp; tmp.name = "Att_Luck_Gear"; tmp.handle = g_build.luck_handle; tmp.pawn = player_pawn(); attr_remove(tmp); g_build.luck_handle.clear();
        DeleteFileA(luck_file().c_str());
    }
    g_build.active = false; g_build.result = r; toast(r.value("ok", false) ? "Builder: " + r["data"].value("summary", std::string("done")) : "Builder: " + r.value("message", std::string("failed")));
    plog("build " + r.dump());
}
static void build_tick() {
    if (!g_luck_orphan.handle.empty() && player_pawn()) {   // gear luck a build of the previous copy left on the player
        g_luck_orphan.name = "Att_Luck_Gear"; attr_remove(g_luck_orphan); DeleteFileA(luck_file().c_str()); plog("build: removed the gear luck of an unfinished build");
    }
    if (!g_build.active) return;
    g_pick_scan.want();   // the keeper steps the shared sliced pickup scan
    const bool pass_done = g_pick_scan.passes != g_build.seen_pass; g_build.seen_pass = g_pick_scan.passes;
    const ULONGLONG now = GetTickCount64();
    if (g_build.need_base) {   // a whole pass that started after the request = what was already on the ground
        if (g_pick_scan.passes >= g_build.base_pass + 2) { for (uintptr_t x : g_pick_scan.live()) g_build.before[x] = pickup_iid(x); g_build.need_base = false; build_spawn_now(); }
        return;
    }
    if (pass_done && g_pick_scan.passes >= g_build.spawn_pass + 1) {
        for (auto i = g_build.before.begin(); i != g_build.before.end();) i = ue::alive(i->first) ? std::next(i) : g_build.before.erase(i);   // gone
        for (uintptr_t x : g_pick_scan.live()) {
            if (build_known(x) || std::find(g_build.fresh.begin(), g_build.fresh.end(), x) != g_build.fresh.end()) continue;
            const std::string ty = lower(pickup_info(x).value("type", std::string()));
            if (ty.empty() || ty.rfind("ammo", 0) == 0 || ty.find("currency") != std::string::npos || ty.find("mission") != std::string::npos ||
                ty.find("health") != std::string::npos || ty.find("eridium") != std::string::npos) { g_build.before[x] = pickup_iid(x); continue; }   // not gear
            // only what lies near the spot the roll was spawned at is the roll: an enemy's / boss's drop elsewhere in the
            // level is not ours (it used to be destroyed as a "wrong rarity" roll)
            { V3 pv; if (!actor_loc_fast(x, pv)) continue;
              if (std::hypot(pv.x - g_build.at.x, pv.y - g_build.at.y) > 1200.0) { g_build.before[x] = pickup_iid(x); continue; } }
            g_build.fresh.push_back(x); g_build.seen_at = now; }
    }
    if (g_build.fresh.empty()) {
        if (now - g_build.spawned_at > 4000) {
            ++g_build.empty;
            if (g_build.tries >= g_build.max_tries || (g_build.empty >= 3 && g_build.level >= 60)) { build_finish(fail("NO_DROP", "the pool " + g_build.pool + " drops nothing, even at level 60")); return; }
            if (g_build.empty >= 2) {   // two empty rolls in a row at this level: this pool needs a higher level
                g_build.level = std::min(60, g_build.level + 5); g_build.empty = 0;
                toast("Builder: nothing drops at this level - trying level " + std::to_string(g_build.level));
            }
            build_spawn();
        }
        return;
    }
    if (now - g_build.seen_at < 400) return;   // let the whole drop arrive
    g_build.empty = 0;
    uintptr_t keep = 0; json info;
    for (uintptr_t x : g_build.fresh) { json i = pickup_info(x); if (!keep && (g_build.want == 0 || i.value("rarity", 0) == g_build.want)) { keep = x; info = i; } }
    if (keep) {
        for (uintptr_t x : g_build.fresh) if (x != keep && ue::alive(x)) ue::call_json(x, "K2_DestroyActor", json::object(), 0);
        bring({ keep }, 220.0);
        static const char* rn[] = { "any", "common", "uncommon", "rare", "epic", "legendary", "pearl" };
        const int r = info.value("rarity", 0);
        // readable: "Epic Daedalus Assault rifle" / "Legendary Kaos (Daedalus Assault rifle)"
        std::string nice; {
            static const std::map<std::string, std::string> mk = { {"bor", "Ripper"}, {"dad", "Daedalus"}, {"jak", "Jakobs"}, {"mal", "Maliwan"},
                {"ord", "Order"}, {"ted", "Tediore"}, {"tor", "Torgue"}, {"vla", "Vladof"} };
            static const std::map<std::string, std::string> ty = { {"Weapon_AR", "Assault rifle"}, {"Weapon_PS", "Pistol"}, {"Weapon_SG", "Shotgun"},
                {"Weapon_SM", "SMG"}, {"Weapon_SR", "Sniper rifle"}, {"Weapon_HW", "Heavy weapon"} };
            static const char* rw[] = { "", "Common", "Uncommon", "Rare", "Epic", "Legendary", "Pearl" };
            const std::string pl = lower(g_build.pool); std::string maker;
            if (pl.rfind("itempool_", 0) == 0 && pl.size() > 12) { auto m = mk.find(pl.substr(9, 3)); if (m != mk.end()) maker = m->second; }
            auto t = ty.find(info.value("type", std::string())); const std::string tname = t != ty.end() ? t->second : info.value("type", std::string("item"));
            std::string uniq; { const size_t k = g_build.pool.find("_legendary_"); if (k != std::string::npos) { uniq = g_build.pool.substr(k + 11); const size_t sh = uniq.find("_shiny"); if (sh != std::string::npos) uniq.resize(sh); } }
            const std::string base = (maker.empty() ? "" : maker + " ") + tname;
            nice = std::string(rw[std::clamp(r, 0, 6)]) + " " + (uniq.empty() ? base : uniq + " (" + base + ")");
            if (nice[0] == ' ') nice.erase(0, 1);
        }
        build_finish(ok({ {"pool", g_build.pool}, {"type", info.value("type", "")}, {"comp", info.value("comp", "")}, {"rarity", rn[std::clamp(r, 0, 6)]},
                          {"tries", g_build.tries}, {"level", g_build.level}, {"summary", nice + ", level " + std::to_string(g_build.level) + " (" + std::to_string(g_build.tries) + " rolls)" +
                              (g_build.level > g_build.asked_level ? " - level raised from " + std::to_string(g_build.asked_level) + ": this item does not drop lower" : std::string())} }));
        return;
    }
    for (uintptr_t x : g_build.fresh) if (ue::alive(x)) ue::call_json(x, "K2_DestroyActor", json::object(), 0);   // wrong rarity: re-roll
    if (g_build.tries >= g_build.max_tries) { build_finish(fail("NO_MATCH", "no " + std::to_string(g_build.want) + "-rarity item in " + std::to_string(g_build.tries) + " rolls - try a rarity pool (leave maker empty)")); return; }
    build_spawn();
}
static json build_cmd(const json& a0) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    if (a0.value("cancel", false)) { if (!g_build.active) return ok({ {"cancelled", false} }); build_finish(fail("CANCELLED", "build stopped")); return ok({ {"cancelled", true} }); }
    if (g_build.active) return fail("BUSY", "a build is running (Stop build to cancel it)");
    json a = g_builder; for (auto& kv : a0.items()) a[kv.key()] = kv.value();
    const json& cat = builder_cat();
    const std::string maker = lower(a.value("maker", std::string())), type = lower(a.value("type", std::string("sg"))), leg = a.value("legendary", std::string());
    const int rar = a.value("rarity", 0);
    std::string pool; int want = 0;
    if (!leg.empty()) pool = leg.rfind("itempool", 0) == 0 || leg.rfind("ItemPool", 0) == 0 ? leg : pool_for_item(leg, a.value("shiny", true));
    else if (!maker.empty() && rar == 5) {
        // legendary + maker + type: the maker pools almost never roll legendary -> one of that combo's named legendaries
        std::vector<std::string> ls;
        for (auto& l : cat.value("legendaries", json::array())) if (l.value("maker", "") == maker && l.value("type", "") == type) ls.push_back(l.value("pool", ""));
        if (ls.empty()) return fail("NO_LEGENDARY", "no named legendary for " + maker + " " + type);
        pool = ls[GetTickCount64() % ls.size()];
    } else if (!maker.empty() && rar != 6) {
        const json combos = cat.value("combos", json::object());
        if (!combos.contains(maker + "_" + type)) return fail("NO_COMBO", maker + " does not make " + type + " (BL.BUILDER lists the combinations)");
        pool = combos[maker + "_" + type].value("pool", std::string()); want = rar;
    } else if (rar > 0) {   // also pearl (6): pearls exist per weapon type only, the maker is ignored
        const json rp = cat.value("rarity_pools", json::object()); const std::string k = type + ":" + std::to_string(rar);
        if (!rp.contains(k)) return fail("NO_POOL", "no " + type + " pool for that rarity"); pool = rp[k].get<std::string>();
    } else { const json ap = cat.value("any_pools", json::object()); if (!ap.contains(type)) return fail("NO_POOL", type); pool = ap[type].get<std::string>(); }
    if (pool.empty()) return fail("NO_POOL", "no pool for that choice");
    g_build = BuildJob(); g_build.active = true; g_build.pool = pool; g_build.want = want; g_build.level = a.value("level", 0) > 0 ? std::clamp(a.value("level", 0), 1, 100) : player_level();   // 0 = your level
    g_build.max_tries = std::clamp(a.value("max_tries", 60), 1, 200);
    if (want >= 4) {   // epic+ from a maker pool: raise gear luck for the rolls (removed when the job ends)
        uintptr_t pawn = player_pawn();
        json r = ue::call_json(attr_lib(), "AddModifierToGbxAttribute", { {"attribute", "def:(\"Att_Luck_Gear\",\"GbxAttributeDef\")"}, {"ContextSource", "addr:" + hexstr(pawn)}, {"ModifierType", 2}, {"modifiervalue", 2000.0}, {"bAutoRefresh", true} }, 0);
        const std::string hx = r.value("ok", false) ? r["data"]["ReturnValue"].value("bytes", std::string()) : "";
        for (size_t i = 0; i + 1 < hx.size(); i += 3) g_build.luck_handle.push_back((uint8_t)std::stoi(hx.substr(i, 2), nullptr, 16));
        if (!g_build.luck_handle.empty())
            std::ofstream(luck_file()) << json({ {"pid", (uint64_t)GetCurrentProcessId()}, {"handle", hx}, {"pawn", hexstr(pawn)} }).dump();
    }
    g_build.empty = 0; g_build.asked_level = g_build.level;
    build_spawn();
    return ok({ {"started", true}, {"pool", pool}, {"want_rarity", want}, {"level", g_build.level} });
}
static json builder_info(const json& a) {   // catalog + current choice; {parts:true} adds the part list of the chosen combo
    const json& cat = builder_cat(); json out = { {"choice", g_builder}, {"busy", g_build.active}, {"last", g_build.result} };
    const std::string k = lower(g_builder.value("maker", std::string())) + "_" + lower(g_builder.value("type", std::string()));
    const json combos = cat.value("combos", json::object());
    out["makers_for_type"] = json::array(); for (auto& kv : combos.items()) if (kv.value().value("type", "") == lower(g_builder.value("type", std::string()))) out["makers_for_type"].push_back(kv.value().value("maker", ""));
    if (combos.contains(k)) { json c = combos[k]; json slots = json::object(); for (auto& s : c["parts"].items()) slots[s.key()] = s.value().size(); out["combo"] = { {"pool", c["pool"]}, {"slots", slots} };
        if (a.value("parts", false)) out["parts"] = c["parts"], out["comps"] = c["comps"]; }
    return ok(out);
}

// ------------------------------------------------------------------ vault cards (only tracks the character already has unlocked)
// Some Vault Card tracks belong to paid content: the trainer never activates or buys a card. It only sets the level
// (OakPlayerState::BP_SetExperienceLevel "VaultCard0N_Experience") and adds tokens (GiveCurrency "VaultCard0N_Tokens")
// for a card whose ExperienceState entry says bIsUnlocked.
static json vaultcard_cmd(const json& a) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    uintptr_t ps = player_ps(); uintptr_t pc = player_pc(); if (!ps || !pc) return fail("NO_PS", "no player");
    // which tracks are unlocked: ExperienceState entries (0x60): level @0, SName ExperienceId @0x28 (FName @+0xC), bIsUnlocked @0x50
    uintptr_t base = 0; ue::Prop arr; if (!ue::resolve(ps, ue::class_of(ps), "ExperienceState", base, arr)) return fail("LAYOUT", "ExperienceState");
    auto ar = ue::array_of(base, arr); json cards = json::object();
    for (int32_t i = 0; i < ar.num && i < 32; ++i) {
        const uintptr_t e = (uintptr_t)ar.data + (uintptr_t)i * 0x60;
        ue::FName n{}; ue::rd_as(e + 0x28 + 0xC, n); const std::string nm = ue::name_str(n);
        if (nm.rfind("VaultCard", 0) != 0) continue;
        int32_t lvl = 0; uint8_t un = 0; ue::rd_as(e, lvl); ue::rd_as(e + 0x50, un);
        cards[nm.substr(0, 11)] = { {"level", lvl}, {"unlocked", un != 0} };
    }
    if (!a.contains("card")) return ok({ {"cards", cards} });
    if (!a["card"].is_number()) return fail("BAD_ARG", "card: 1..99");
    const int c = (int)std::clamp(a["card"].get<double>(), -1.0, 1000.0);
    if (c < 1 || c > 99) return fail("BAD_ARG", "card: 1..99");   // a bigger number overflowed the id buffer (sprintf_s ends the process)
    char id[16]; snprintf(id, sizeof id, "VaultCard%02d", c);
    if (!cards.contains(id)) return fail("NO_CARD", std::string(id) + " not on this character");
    if (!cards[id].value("unlocked", false)) return fail("LOCKED", std::string(id) + " is not unlocked on this character - the trainer does not unlock Vault Cards (some are paid content)");
    json out = { {"card", id} };
    if (a.contains("level")) {
        json r = ue::call_json(ps, "BP_SetExperienceLevel", { {"ExperienceDef", std::string("def:(\"") + id + "_Experience\",\"GbxExperienceDef\")"}, {"ExperienceLevel", a["level"].get<int>()} }, 0);
        out["level"] = r.value("ok", false) ? json(a["level"]) : json(r.value("message", std::string()));
    }
    if (a.contains("tokens")) {
        json r = ue::call_json(ue::cdo_of(ue::find_class("GbxCurrencyFunctionLibrary")), "GiveCurrency",
            { {"OwnerContext", "addr:" + hexstr(pc)}, {"currency", std::string("def:(\"") + id + "_Tokens\",\"GbxCurrencyDef\")"}, {"Amount", a["tokens"].get<int>()} }, 0);
        out["tokens"] = r.value("ok", false) ? json(a["tokens"]) : json(r.value("message", std::string()));
        out["balance"] = currency_amount(std::string(id) + "_Tokens");
    }
    plog("vault card " + out.dump()); return ok(out);
}


// ------------------------------------------------------------------ skill points / specialization tokens / SDU tokens / respec
// OakCharacter.GbxProgressionManager.ProgressPointsContainer.PointsAcquiredPerPool = int32[3]: points EARNED per pool
// (0 skill points, 1 specialization tokens, 2 SDU tokens); what is spent lives in the progress graphs, so raising an
// earned count gives that many unspent points, shown at once in the Skills / SDU menus (memory write - no game setter).
// Respec: GbxProgressGraph::Server_ResetSpentPoints on the character's skill-tree graphs (the game's own refund).
// The game rebuilds skill points / specialization tokens from the level (level up, load), so a raised count did not last.
// What was ADDED is kept per character (<data>\points_bonus.json, key "<n>.sav|<name>") and pts_tick adds it again on top
// of the game's own count whenever that count changes. "live" = the values last written by this game process, so a
static json g_pts = json::object();            // {bonus: {key: {skill_points, spec_tokens}}, live: {pid, key, w: [a, b]}}
static std::string g_pts_key; static uintptr_t g_pts_pawn = 0; static int32_t g_pts_w[2] = { -1, -1 };
static std::string pts_file() { return g_data_dir + "\\points_bonus.json"; }
static void pts_save() {
    g_pts["live"] = { {"pid", (uint64_t)GetCurrentProcessId()}, {"key", g_pts_key}, {"w", { g_pts_w[0], g_pts_w[1] }} };
    std::string err; const std::string t = pts_file() + ".tmp";
    { std::ofstream o(t, std::ios::trunc); if (!o) return; o << g_pts.dump(1); }
    MoveFileExA(t.c_str(), pts_file().c_str(), MOVEFILE_REPLACE_EXISTING);
}
static void pts_load_once() {
    static bool done = false; if (done) return; done = true;
    try { std::ifstream f(pts_file()); if (f) g_pts = json::parse(f); } catch (...) { g_pts = json::object(); }
    if (!g_pts.is_object()) g_pts = json::object();
    if (!g_pts.contains("bonus") || !g_pts["bonus"].is_object()) g_pts["bonus"] = json::object();
}
static bool pts_slots(uintptr_t pawn, uintptr_t out[3]) {
    uintptr_t base = 0; ue::Prop arr;
    if (!ue::resolve(pawn, ue::class_of(pawn), "GbxProgressionManager.ProgressPointsContainer.PointsAcquiredPerPool", base, arr)) return false;
    auto ar = ue::array_of(base, arr); if (ar.num < 3 || !ar.data) return false;
    for (int i = 0; i < 3; ++i) out[i] = (uintptr_t)ar.data + i * 4;
    return true;
}
static std::string pts_char_key() {   // the selected character's save file + name, read from the save (read only)
    json r = sv::cmd("TQ.SAVE", json::object()); if (!r.value("ok", false) || !r["data"].is_object()) return "";
    const json& d = r["data"]; const std::string f = d.value("file", std::string()); if (f.empty()) return "";
    std::string n; if (d.contains("name") && d["name"].is_string()) n = d["name"].get<std::string>();
    return f + "|" + n;
}
static bool pts_bind(uintptr_t pawn) {   // new player character: find whose it is, adopt what this process already wrote
    if (pawn == g_pts_pawn && !g_pts_key.empty()) return true;
    pts_load_once();
    const std::string k = pts_char_key(); if (k.empty()) return false;
    g_pts_pawn = pawn; g_pts_key = k; g_pts_w[0] = g_pts_w[1] = -1;
    const json lv = g_pts.value("live", json::object());
    if (lv.is_object() && lv.value("pid", (uint64_t)0) == (uint64_t)GetCurrentProcessId() && lv.value("key", std::string()) == k && lv.contains("w") && lv["w"].is_array() && lv["w"].size() == 2)
        for (int i = 0; i < 2; ++i) g_pts_w[i] = lv["w"][i].is_number() ? lv["w"][i].get<int32_t>() : -1;
    return true;
}
static int pts_bonus(int i) {
    static const char* kB[2] = { "skill_points", "spec_tokens" };
    const json b = g_pts["bonus"].value(g_pts_key, json::object());
    return b.is_object() && b.contains(kB[i]) && b[kB[i]].is_number() ? b[kB[i]].get<int>() : 0;
}
// Skill tree keeper. When the game saves or loads it recounts the pools from the level and DROPS every spend above that
// count (seen 2026-11-16: 8 points spent at level 3 -> the save holds none of them, pool saved as 2). So the trainer
// remembers what each node has (g_pts["trees"][key][graph][node] = points) and, after every recount, spends the missing
// points again with the game's own GbxProgressGraph::Server_SpendProgressPoints. The save itself always stays the game's
// own valid state. A lower reading is only remembered after it held for 3 checks (a respec), so a drop that comes a
// moment before its recount is not taken for the new state.
static ULONGLONG g_tree_restore_at = 0;
static bool tree_graph_ok(const std::string& lo) {   // skill trees + specializations; never SDU / vault power / vault cards
    return lo.find("progress") != std::string::npos && lo.find("sdu") == std::string::npos && lo.find("vaultpower") == std::string::npos
        && lo.find("vaultcard") == std::string::npos;
}
// graph def text -> {graph object, node index -> points spent}
static std::map<std::string, std::pair<uintptr_t, std::map<int, int>>> tree_read(uintptr_t pawn) {
    std::map<std::string, std::pair<uintptr_t, std::map<int, int>>> out;
    uintptr_t gb = 0; ue::Prop garr;
    if (!ue::resolve(pawn, ue::class_of(pawn), "GbxProgressionManager.ProgressGraphs", gb, garr)) return out;
    auto g = ue::array_of(gb, garr);
    for (int32_t i = 0; i < g.num && i < 64; ++i) {
        uintptr_t go = 0; ue::rd_as((uintptr_t)g.data + i * 8, go); if (!go) continue;
        ue::Prop dp = ue::find_prop(ue::class_of(go), "ProgressGraphDef"); if (!dp) continue;
        const std::string nm = ue::def_text(go + dp.offset); if (!tree_graph_ok(lower(nm))) continue;
        ue::Prop np = ue::find_prop(ue::class_of(go), "nodes"); if (!np) continue;
        auto na = ue::array_of(go, np); auto& e = out[nm]; e.first = go;
        for (int32_t k = 0; k < na.num && k < 256; ++k) { int32_t sp = 0; ue::rd_as((uintptr_t)na.data + k * 0xA8 + 0x94, sp); if (sp > 0) e.second[k] = sp; }
    }
    return out;
}
static void tree_tick(uintptr_t pawn, ULONGLONG now) {
    if (g_pts_key.empty()) return;
    if (!g_pts.contains("trees") || !g_pts["trees"].is_object()) g_pts["trees"] = json::object();
    json& mem = g_pts["trees"][g_pts_key]; if (!mem.is_object()) mem = json::object();
    auto live = tree_read(pawn); if (live.empty()) return;
    if (g_tree_restore_at) {   // after a recount: spend again what the game dropped (two passes: a tier can need the one before)
        if (now < g_tree_restore_at) return;
        g_tree_restore_at = 0; int put = 0;
        for (int pass = 0; pass < 2; ++pass) {
            for (auto& [gname, nodes] : mem.items()) {
                auto it = live.find(gname); if (it == live.end() || !nodes.is_object()) continue;
                std::vector<std::pair<int, int>> want;
                for (auto& [ks, v] : nodes.items()) if (v.is_number()) want.push_back({ atoi(ks.c_str()), v.get<int>() });
                std::sort(want.begin(), want.end());
                for (auto& [k, pts] : want) {
                    const int have = it->second.second.count(k) ? it->second.second[k] : 0; if (pts <= have) continue;
                    json r = ue::call_json(it->second.first, "Server_SpendProgressPoints", { {"InNodeIndex", k}, {"InPoints", pts - have} }, 0);
                    if (r.value("ok", false)) put += pts - have;
                }
            }
            live = tree_read(pawn);
        }
        if (put) { plog("skill tree: spent " + std::to_string(put) + " point(s) again after the game recount");
            static ULONGLONG told = 0; if (!told || now - told > 60000) { told = now; toast("Skill tree restored (" + std::to_string(put) + " points)"); } }   // autosaves: at most once a minute
        return;
    }
    // remember the live tree: higher readings at once, lower ones only after 3 equal checks in a row (respec)
    static std::string low_sig; static int low_n = 0;
    bool changed = false; std::string sig; bool lower_seen = false;
    for (auto& [gname, e] : live) {
        json now_nodes = json::object(); for (auto& [k, v] : e.second) now_nodes[std::to_string(k)] = v;
        const json old = mem.value(gname, json::object());
        bool lower = false; for (auto& [ks, v] : old.items()) { const int nv = now_nodes.value(ks, 0); if (v.is_number() && nv < v.get<int>()) lower = true; }
        if (lower) { lower_seen = true; sig += gname + now_nodes.dump(); continue; }
        if (now_nodes != old) { if (now_nodes.empty()) mem.erase(gname); else mem[gname] = now_nodes; changed = true; }
    }
    if (lower_seen) {
        if (sig == low_sig) ++low_n; else { low_sig = sig; low_n = 1; }
        if (low_n >= 3) { for (auto& [gname, e] : live) { json n2 = json::object(); for (auto& [k, v] : e.second) n2[std::to_string(k)] = v; if (n2.empty()) mem.erase(gname); else mem[gname] = n2; }
                          changed = true; low_sig.clear(); low_n = 0; plog("skill tree: fewer points kept (respec)"); }
    } else { low_sig.clear(); low_n = 0; }
    if (changed) pts_save();
}
static void pts_tick() {
    static ULONGLONG at = 0; const ULONGLONG now = GetTickCount64(); if (now - at < 250) return; at = now;   // 4x a second: a dropped tree is back within ~0.5 s
    if (!session().value("standalone", false)) return;
    const uintptr_t pawn = player_pawn(); if (!pawn) return;
    pts_load_once();
    if (g_pts["bonus"].empty()) return;   // nothing kept: no save read at all
    if (!pts_bind(pawn)) return;
    uintptr_t slot[3]; if (!pts_slots(pawn, slot)) return;
    bool wrote = false;
    for (int i = 0; i < 2; ++i) {
        const int b = pts_bonus(i); if (b <= 0) continue;
        int32_t v = 0; if (!ue::rd_as(slot[i], v)) continue;
        if (v == g_pts_w[i]) continue;                       // still what we wrote
        const int32_t nv = (int32_t)std::clamp<long long>((long long)v + b, 0, 1000000);   // the game recounted: its count + the bonus
        if (ue::mwrite(slot[i], &nv, 4)) { g_pts_w[i] = nv; wrote = true; plog("points: game count " + std::to_string(v) + " + kept " + std::to_string(b) + " -> " + std::to_string(nv)); }
    }
    if (wrote) { pts_save(); g_tree_restore_at = now + 250; }   // a recount: the game may also have dropped spends above its count
    try { tree_tick(pawn, now); } catch (...) {}
}
static json points_cmd(const json& a) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    const uintptr_t pawn = player_pawn(); if (!pawn) return fail("NO_PAWN", "no player");
    uintptr_t base = 0; ue::Prop arr;
    if (!ue::resolve(pawn, ue::class_of(pawn), "GbxProgressionManager.ProgressPointsContainer.PointsAcquiredPerPool", base, arr)) return fail("LAYOUT", "PointsAcquiredPerPool not found");
    auto ar = ue::array_of(base, arr); if (ar.num < 3 || !ar.data) return fail("LAYOUT", "expected 3 point pools, found " + std::to_string(ar.num));
    static const char* kN[3] = { "skill_points", "spec_tokens", "sdu_tokens" };
    json out = json::object(); std::string msg;
    for (int i = 0; i < 3; ++i) {
        const uintptr_t at = (uintptr_t)ar.data + i * 4; int32_t v = 0; ue::rd_as(at, v);
        if (a.contains(kN[i]) || (a.value("pool", "") == kN[i] && (a.contains("v") || a.contains("add")))) {
            const json& j = a.contains(kN[i]) ? a[kN[i]] : a.contains("v") ? a["v"] : a["add"];
            long long nv = j.is_number() ? j.get<long long>() : atoll(j.get<std::string>().c_str());
            if (a.value("pool", "") == kN[i] && a.contains("add")) nv = v + nv;
            nv = std::clamp<long long>(nv, 0, 1000000);
            int32_t w = (int32_t)nv; if (!ue::mwrite(at, &w, 4)) return fail("WRITE", kN[i]);
            if (i < 2 && !a.value("raw", false) && pts_bind(pawn)) {   // raw: test only - acts like the game's own recount   // remember how far above the game's own count this is, so it survives level ups / loads
                const int old_b = pts_bonus(i);
                const long long game = (v == g_pts_w[i]) ? (long long)v - old_b : v;   // the game's count under our value
                const int nb = (int)std::max<long long>(0, nv - game);
                g_pts["bonus"][g_pts_key][kN[i]] = nb; g_pts_w[i] = w; pts_save();
                msg += std::string(msg.empty() ? "" : ", ") + "kept +" + std::to_string(nb) + " above your level's " + std::to_string(game);
            }
            msg += std::string(msg.empty() ? "" : ", ") + kN[i] + " " + std::to_string(v) + " -> " + std::to_string(w);
            v = w;
        }
        out[kN[i]] = v;
        if (a.value("addr", false)) out[std::string(kN[i]) + "_addr"] = hexstr(at);
    }
    if (a.value("respec", false)) {
        uintptr_t gb = 0; ue::Prop garr; json reset = json::array(); bool forgot = false;
        pts_load_once();
        if (ue::resolve(pawn, ue::class_of(pawn), "GbxProgressionManager.ProgressGraphs", gb, garr)) {
            auto g = ue::array_of(gb, garr);
            const std::string want = a.value("graphs", std::string("skill"));
            for (int32_t i = 0; i < g.num && i < 64; ++i) {
                uintptr_t go = 0; ue::rd_as((uintptr_t)g.data + i * 8, go); if (!go) continue;
                ue::Prop dp = ue::find_prop(ue::class_of(go), "ProgressGraphDef"); if (!dp) continue;
                std::string nm = ue::def_text(go + dp.offset); std::string lo = nm; std::transform(lo.begin(), lo.end(), lo.begin(), ::tolower);
                if (lo.find(want) == std::string::npos || (lo.find("sdu") != std::string::npos && want.find("sdu") == std::string::npos)) continue;   // the SDU tree only when asked for
                json r = ue::call_json(go, "Server_ResetSpentPoints", json::object(), 0);
                reset.push_back(nm + (r.value("ok", false) ? "" : " (failed: " + r.value("message", std::string()) + ")"));
                // 2026-11-18: the tree keeper must forget this tree now - it took the reset for a game recount that
                if (r.value("ok", false) && pts_bind(pawn) && g_pts.contains("trees") && g_pts["trees"].is_object()
                    && g_pts["trees"].contains(g_pts_key) && g_pts["trees"][g_pts_key].is_object()) { g_pts["trees"][g_pts_key].erase(nm); forgot = true; }
            }
        }
        if (forgot) { g_tree_restore_at = 0; pts_save(); plog("skill tree: respec - the keeper forgot the reset trees"); }
        out["respec"] = reset; msg += std::string(msg.empty() ? "" : ", ") + "respec " + std::to_string(reset.size()) + " tree(s)";
    }
    if (a.value("list", false)) {
        uintptr_t gb = 0; ue::Prop garr; json gl = json::array();
        if (ue::resolve(pawn, ue::class_of(pawn), "GbxProgressionManager.ProgressGraphs", gb, garr)) {
            auto g = ue::array_of(gb, garr);
            for (int32_t i = 0; i < g.num && i < 64; ++i) { uintptr_t go = 0; ue::rd_as((uintptr_t)g.data + i * 8, go); if (!go) continue;
                ue::Prop dp = ue::find_prop(ue::class_of(go), "ProgressGraphDef"); if (!dp) continue;
                if (!a.value("nodes", false)) { gl.push_back(ue::def_text(go + dp.offset)); continue; }
                json g1 = { {"graph", ue::def_text(go + dp.offset)}, {"addr", hexstr(go)}, {"nodes", json::array()} };
                ue::Prop np = ue::find_prop(ue::class_of(go), "nodes");
                if (np) { auto na = ue::array_of(go, np);
                    for (int32_t k = 0; k < na.num && k < 256; ++k) { const uintptr_t nd = (uintptr_t)na.data + k * 0xA8;
                        uint8_t un = 0, ac = 0; int32_t sp = 0, bo = 0; ue::rd_as(nd + 0x90, un); ue::rd_as(nd + 0x91, ac); ue::rd_as(nd + 0x94, sp); ue::rd_as(nd + 0x98, bo);
                        if (sp || bo || ac) g1["nodes"].push_back({ {"i", k}, {"spent", sp}, {"bonus", bo}, {"act", ac}, {"unl", un}, {"at", hexstr(nd + 0x94)} }); } }
                gl.push_back(g1); }
        }
        out["graphs"] = gl;
    }
    if (!msg.empty()) plog("points " + msg);
    return ok(out, msg.empty() ? "skill points " + std::to_string(out["skill_points"].get<int>()) + ", spec tokens " + std::to_string(out["spec_tokens"].get<int>()) + ", SDU tokens " + std::to_string(out["sdu_tokens"].get<int>()) : msg);
}


// ------------------------------------------------------------------ DEF: Gearbox design data by name (items, parts, skills, loot, missions, UI text ...)
// Every kind of def lives in a NexusConfigStore* object (one per kind, ~300 classes). Its defs are a hash map at store
// +0x1F8 (or +0x2E8): TSparseArray {data, num, max}, 0x70-byte entries {key u64, ?, def* @+0x10}; key >> 32 = the def's
// FName comparison index (its name). A def's struct type: vtable slot 1 = "GetStructType" (jmp -> sub rsp,28;
// mov rax,[static]) -> the cached UScriptStruct, so every field is readable / writable through reflection.
// Found 2026-11-16 from KismetSystemLibrary::IsValidGbxDefPtr -> type manager (game+0x89A1AB0) -> store vfunc +0x2B0.
struct DefRec { uintptr_t addr = 0, store = 0; std::string name; };
static std::unordered_map<std::string, std::vector<DefRec>> g_def_idx;   // lower-case name -> defs with that name
static std::vector<std::pair<uintptr_t, int32_t>> g_def_stores;           // store, def count
static size_t g_def_total = 0;
static bool def_name_ok(const std::string& s) {
    if (s.empty() || s == "None" || s.size() > 200) return false;
    for (unsigned char c : s) if (c < 0x20 || c > 0x7E) return false;
    return true;
}
// The map's place and entry size depend on the store kind: +0x1C8 ("basic" stores: item pools ... 0x20-byte entries),
// +0x1F8 (inventory, missions, levels ... 0x70), +0x2E8. Whichever decodes to real names + readable defs is used.
static bool def_map(uintptr_t store, uintptr_t& data, int32_t& num, int32_t& stride) {
    for (int off : { 0x1C8, 0x1F8, 0x2E8 }) {
        uintptr_t d = 0; int32_t n = 0, mx = 0; ue::rd_as(store + off, d); ue::rd_as(store + off + 8, n); ue::rd_as(store + off + 12, mx);
        if (!d || n <= 0 || mx < n || mx > 4000000) continue;
        for (int32_t st : { 0x20, 0x70, 0x18, 0x28, 0x30, 0x38, 0x40, 0x48, 0x50, 0x58, 0x60, 0x68, 0x78, 0x80, 0x88, 0x90 }) {
            int good = 0, seen = 0;
            for (int32_t i = 0; i < n && i < 6; ++i, ++seen) {
                uint64_t key = 0; uintptr_t def = 0; const uintptr_t e = d + (uintptr_t)i * st;
                if (!ue::rd_as(e, key) || !key || !ue::rd_as(e + 0x10, def) || def < 0x10000 || def > 0x7FFFFFFFFFFF) continue;
                ue::FName f{}; f.index = (uint32_t)(key >> 32); f.number = 0;
                uintptr_t vt = 0; if (def_name_ok(ue::name_str(f)) && ue::rd_as(def, vt) && vt > 0x10000) ++good;
            }
            if (good >= std::min(3, seen) && good > 0) { data = d; num = n; stride = st; return true; }
        }
    }
    return false;
}
static std::string def_name_of(uint64_t key) { ue::FName f{}; f.index = (uint32_t)(key >> 32); f.number = 0; return ue::name_str(f); }
static uintptr_t def_struct(uintptr_t def) { return ue::typed_struct_of(def); }
[[maybe_unused]] static uintptr_t def_struct_old(uintptr_t def) {   // vtable[1]: jmp rel32 -> sub rsp,28 ; mov rax,[rip+disp] -> UScriptStruct*
    uintptr_t vt = 0, fn = 0; if (!ue::rd_as(def, vt) || !vt || !ue::rd_as(vt + 8, fn) || !fn) return 0;
    uint8_t b[16] = {}; if (!ue::mread(fn, b, sizeof b)) return 0;
    if (b[0] == 0xE9) { int32_t rel = 0; memcpy(&rel, b + 1, 4); fn = fn + 5 + rel; if (!ue::mread(fn, b, sizeof b)) return 0; }
    if (!(b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 && b[5] == 0x8B && b[6] == 0x05)) return 0;
    int32_t disp = 0; memcpy(&disp, b + 7, 4); const uintptr_t stat = fn + 4 + 7 + disp;
    uintptr_t st = 0; if (!ue::rd_as(stat, st) || !st || !ue::alive(st)) return 0;
    return ue::name_of(ue::class_of(st)) == "ScriptStruct" ? st : 0;
}
static void def_index_build() {
    g_def_idx.clear(); g_def_stores.clear(); g_def_total = 0;
    const uintptr_t base = ue::find_class("NexusConfigStore"); if (!base) return;
    for (uintptr_t s : ue::find_all(base, false)) {
        uintptr_t data = 0; int32_t num = 0, stride = 0x70; if (!def_map(s, data, num, stride)) continue;
        int32_t got = 0;
        for (int32_t i = 0; i < num && i < 2000000; ++i) {
            uint64_t key = 0; uintptr_t def = 0; const uintptr_t e = data + (uintptr_t)i * stride;
            if (!ue::rd_as(e, key) || !key || !ue::rd_as(e + 0x10, def) || def < 0x10000) continue;
            const std::string nm = def_name_of(key); if (!def_name_ok(nm)) continue;
            g_def_idx[lower(nm)].push_back({ def, s, nm }); ++got;
        }
        g_def_stores.push_back({ s, got }); g_def_total += got;
    }
}
static std::string store_kind(uintptr_t s) { std::string n = ue::name_of(ue::class_of(s)); return n.rfind("NexusConfigStore", 0) == 0 ? n.substr(16) : n; }
// In-game names: InventoryNamePartDef "np_<GearboxName>" .PartName = the words the game shows ("np_Extinction" ->
// "Eradication"). Read from the live defs once.
static std::unordered_map<std::string, std::string> g_disp;   // lower in-game name -> Gearbox name
static void disp_build() {
    if (!g_disp.empty()) return;
    if (g_def_idx.empty()) def_index_build();
    for (auto& [k, recs] : g_def_idx) for (auto& r : recs) {
        if (store_kind(r.store) != "InventoryNamePart" || r.name.size() < 4 || lower(r.name.substr(0, 3)) != "np_") continue;
        const uintptr_t st = def_struct(r.addr); if (!st) continue;
        uintptr_t b = 0; ue::Prop leaf; if (!ue::resolve(r.addr, st, "PartName", b, leaf) || leaf.type != "TextProperty") continue;
        const std::string shown = ue::ftext(b + leaf.offset); if (shown.empty()) continue;
        g_disp.emplace(lower(shown), r.name.substr(3));
    }
}
static std::string display_to_internal(const std::string& q) {
    disp_build(); std::string lq = lower(q);
    while (!lq.empty() && lq.back() == ' ') lq.pop_back();
    auto it = g_disp.find(lq); if (it != g_disp.end()) return it->second;
    for (auto& [shown, in] : g_disp) if (lq.size() >= 4 && shown.find(lq) != std::string::npos) return in;   // part of the name
    return "";
}
static json display_names_cmd(const json& a) {   // {filter}: in-game name -> Gearbox name
    disp_build(); const std::string f = lower(a.value("filter", std::string())); json out = json::object();
    for (auto& [shown, in] : g_disp) if (f.empty() || shown.find(f) != std::string::npos || lower(in).find(f) != std::string::npos) out[shown] = in;
    return ok({ {"count", out.size()}, {"names", out} });
}
// ---- balance tables (GbxUEDataTableDefs): most real numbers (perk chances, unique damage, scaling) live here.
// GbxUEDataTableDef: +0x48 row struct (UserDefinedStruct), +0x50 rows {data, num @+0x58}: 0x18-byte entries
// {FName row, uint8* row data, hash}. Column names carry a GUID ("Value_1_68_67DA...") - the short name "Value_1" works.
static bool table_rows(uintptr_t def, uintptr_t& st, std::vector<std::pair<std::string, uintptr_t>>& rows) {
    rows.clear(); st = 0; if (!ue::rd_as(def + 0x48, st) || !st || !ue::alive(st)) return false;
    uintptr_t data = 0; int32_t num = 0; ue::rd_as(def + 0x50, data); ue::rd_as(def + 0x58, num);
    if (!data || num < 0 || num > 200000) return false;
    for (int32_t i = 0; i < num; ++i) {
        ue::FName f{}; uintptr_t row = 0; const uintptr_t e = data + (uintptr_t)i * 0x18;
        if (!ue::rd_as(e, f) || !ue::rd_as(e + 8, row) || !row) continue;
        const std::string n = ue::name_str(f); if (n.empty() || n == "None") continue;
        rows.push_back({ n, row });
    }
    return true;
}
static std::string short_col(const std::string& n) {   // "Value_1_68_67DA482B..." -> "Value_1" (the editor's GUID suffix off)
    const size_t g = n.size() > 33 ? n.rfind('_') : std::string::npos;
    if (g == std::string::npos || n.size() - g - 1 != 32) return n;
    const size_t k = n.rfind('_', g - 1); return k == std::string::npos ? n.substr(0, g) : n.substr(0, k);
}
static ue::Prop table_col(uintptr_t st, const std::string& want, std::string& full) {
    const std::string w = lower(want);
    for (auto& p : ue::props(st, true)) if (lower(p.name) == w || lower(short_col(p.name)) == w) { full = p.name; return p; }
    return ue::Prop();
}
static json table_cmd(const std::string& name, const json& a, const std::function<uintptr_t(const std::string&)>& find_table) {
    const std::string tn = a.value("table", std::string()); if (tn.empty()) return fail("BAD_ARG", "table (a GbxUEDataTableDefs name)");
    const uintptr_t def = find_table(tn); if (!def) return fail("NOT_FOUND", "no balance table " + tn + " (BL.DEF_FIND {store: \"GbxUEDataTableDefs\"})");
    uintptr_t st = 0; std::vector<std::pair<std::string, uintptr_t>> rows; if (!table_rows(def, st, rows)) return fail("LAYOUT", "table rows not readable");
    const std::string rw = lower(a.value("row", std::string()));
    auto clean = [](json o) { json c = json::object(); if (o.is_object()) for (auto& [k, v] : o.items()) c[short_col(k)] = v; return c; };
    if (name == "TQ.TABLE_GET") {   // {table, row?, column?, filter?, max?}
        json out = { {"table", tn}, {"struct", ue::name_of(st)}, {"rows_total", rows.size()} };
        if (rw.empty()) {
            json cols = json::array(); for (auto& p : ue::props(st, true)) cols.push_back(short_col(p.name)); out["columns"] = cols;
            const std::string f = lower(a.value("filter", std::string())); const size_t mx = a.value("max", 50); json rs = json::object();
            for (auto& [n, r] : rows) { if (!f.empty() && lower(n).find(f) == std::string::npos) continue; if (rs.size() >= mx) break; rs[n] = clean(ue::struct_json(r, st, 0)); }
            out["rows"] = rs; return ok(out);
        }
        for (auto& [n, r] : rows) if (lower(n) == rw) {
            const std::string c = a.value("column", std::string());
            if (c.empty()) { out["row"] = n; out["values"] = clean(ue::struct_json(r, st, 0)); return ok(out); }
            std::string full; ue::Prop p = table_col(st, c, full); if (!p) return fail("NO_COLUMN", c);
            out["row"] = n; out["column"] = short_col(full); out["value"] = ue::get_num(r, p); out["type"] = p.type; return ok(out);
        }
        return fail("NO_ROW", a.value("row", std::string()));
    }
    if (name == "TQ.TABLE_SET") {   // {table, row, column, value}: in memory until the game restarts
        json s = session(); if (!s.value("standalone", false) && !a.value("menu_ok", false)) return fail("NOT_SOLO", "solo only");
        if (!a.contains("value")) return fail("BAD_ARG", "value");
        for (auto& [n, r] : rows) if (lower(n) == rw) {
            std::string full; ue::Prop p = table_col(st, a.value("column", std::string()), full); if (!p) return fail("NO_COLUMN", a.value("column", std::string()));
            json res = ue::field_set(r, st, full, a["value"]);
            plog("table set " + tn + "[" + n + "]." + short_col(full) + " -> " + a["value"].dump());
            if (res.value("ok", false)) res["data"]["field"] = short_col(full);
            return res;
        }
        return fail("NO_ROW", a.value("row", std::string()));
    }
    return fail("UNKNOWN", name);
}
// ---- a part's perks and the numbers behind them (Gear Maker value editor)
// InventoryPartDef.Aspects: UIStatAspect.UIStatsToInclude -> UIStatDisplayDef (card text, {placeholders} from
// NumericDisplayValue.AttributeDef) -> GbxAttributeDef.Value (resolver) -> balance table cell or constant.
// AttributeEffectAspect / WeaponAttributeEffectAspect: attribute modifiers whose value is a GbxAttributeInit
// (DataTableValue {DataTable, RowName, ColumnName} or constant).
static uintptr_t def_find(const std::string& n, const char* store) {
    auto it = g_def_idx.find(lower(n)); if (it == g_def_idx.end()) return 0;
    for (auto& r : it->second) if (!store || store_kind(r.store) == store) return r.addr;
    return 0;
}
static std::string def_name_in(const std::string& t) {   // "(\"name\",\"Type\")" / "gdh:name" -> name
    if (t.rfind("gdh:", 0) == 0) return t.substr(4);
    const size_t a = t.find('"'); if (a == std::string::npos) return t; const size_t b = t.find('"', a + 1);
    return b == std::string::npos ? t : t.substr(a + 1, b - a - 1);
}
// where a GbxAttributeInit-like value comes from: {kind: table, table, row, column, value} | {kind: constant, value}
static json value_source(const json& init) {
    if (!init.is_object()) return nullptr;
    const json dv = init.value("DataTableValue", json::object());
    const std::string tbl = def_name_in(dv.value("DataTable", std::string()));
    const std::string row = dv.value("RowName", std::string()), col = dv.value("ColumnName", std::string());
    if (!tbl.empty() && tbl != "None" && row != "None" && col != "None") {
        json out = { {"kind", "table"}, {"table", tbl}, {"row", row}, {"column", short_col(col)} };
        if (uintptr_t td = def_find(tbl, "GbxUEDataTableDefs")) { uintptr_t st = 0; std::vector<std::pair<std::string, uintptr_t>> rows;
            if (table_rows(td, st, rows)) for (auto& [n, r] : rows) if (n == row) { std::string full; ue::Prop p = table_col(st, col, full); if (p) out["value"] = ue::get_num(r, p); } }
        if (init.contains("PostScale") && init["PostScale"].is_number() && init["PostScale"].get<double>() != 1.0) out["post_scale"] = init["PostScale"];
        return out;
    }
    if (init.contains("attribute") && init["attribute"].is_string()) return json{ {"kind", "attribute"}, {"attribute", def_name_in(init["attribute"].get<std::string>())} };
    if (init.contains("constant") && init["constant"].is_number()) return json{ {"kind", "constant"}, {"value", init["constant"]} };
    return nullptr;
}
static json attr_source(const std::string& attr) {   // GbxAttributeDef -> where its value comes from
    const uintptr_t ad = def_find(attr, "AttributeDefs"); if (!ad) return nullptr;
    const uintptr_t st = def_struct(ad); if (!st) return nullptr;
    uintptr_t b = 0; ue::Prop leaf; if (!ue::resolve(ad, st, "Value", b, leaf)) return nullptr;
    uintptr_t inst = 0, ist = 0; if (!ue::inline_target(b + leaf.offset, inst, ist)) return nullptr;
    const std::string kind = ue::name_of(ist); json v = ue::struct_json(inst, ist, 0, 5);
    json out = { {"resolver", kind} };
    if (kind == "GbxDataTableValueResolver") { json s = value_source(v); if (s.is_object()) out.update(s); }
    else if (kind == "GbxConstantAttributeValueResolver") { json s = value_source(v.value("AttributeInit", json::object())); if (s.is_object()) out.update(s); }
    else if (kind == "BalanceFormulaValueResolver") { json s = value_source(v.value("Level", json::object())); if (s.is_object()) { out.update(s); out["formula"] = "Level (this) x Multiplier ^ Power + Offset"; } }
    out["attr_def"] = attr;
    return out;
}
static json part_info(uintptr_t part) {
    const uintptr_t st = def_struct(part); if (!st || ue::name_of(st) != "InventoryPartDef") return fail("NOT_A_PART", "that part is not loaded - rebuild the item database after a game restart");
    json full = ue::struct_json(part, st, 0, 7);
    json perks = json::array(), effects = json::array();
    for (auto& asp : full.value("Aspects", json::array())) {
        if (!asp.is_object()) continue;
        const std::string ty = asp.value("@type", std::string());
        if (ty == "UIStatAspect") for (auto& sref : asp.value("UIStatsToInclude", json::array())) {
            if (!sref.is_string()) continue; const std::string sn = def_name_in(sref.get<std::string>());
            const uintptr_t sd = def_find(sn, "_StatDisplay"); if (!sd) continue; const uintptr_t sst = def_struct(sd); if (!sst) continue;
            uintptr_t b = 0; ue::Prop leaf; if (!ue::resolve(sd, sst, "StatValue", b, leaf)) continue;
            uintptr_t inst = 0, ist = 0; if (!ue::inline_target(b + leaf.offset, inst, ist)) continue;
            json sv = ue::struct_json(inst, ist, 0, 5);
            std::string text = sv.value("FormatText", std::string());
            for (size_t k; (k = text.find('[')) != std::string::npos; ) { const size_t e = text.find(']', k); if (e == std::string::npos) break; text.erase(k, e - k + 1); }
            json args = json::object();
            auto add_arg = [&](const std::string& key, const json& nv) {
                const json ad = nv.value("AttributeDef", json());
                if (!ad.is_string()) return;
                json src = attr_source(def_name_in(ad.get<std::string>()));
                json a1 = { {"attribute", def_name_in(ad.get<std::string>())}, {"percent", nv.value("bDisplayAsPercentage", false)},
                            {"one_plus", nv.value("bDisplayAsOnePlus", false)}, {"one_minus", nv.value("bDisplayAsOneMinus", false)},
                            {"inverse", nv.value("bDisplayAsInverse", false)}, {"reduction", nv.value("bCalculateWithReductionMath", false)},
                            {"plus_sign", nv.value("bDisplayPlusSign", false)}, {"pct_as_float", nv.value("bDisplayPercentAsFloat", false)},
                            {"precision", nv.value("FloatPrecision", 0.0)} };   // the card's display rules
                if (src.is_object()) a1["source"] = src;
                args[key] = a1; };
            const json am = sv.value("ArgsMap", json());
            if (am.is_object()) { for (auto& [k, v] : am.items()) if (v.is_object()) add_arg(k, v); }
            else if (sv.contains("AttributeDef")) add_arg("value", sv);
            perks.push_back({ {"stat", sn}, {"text", text}, {"args", args} });
        }
        if (ty == "AttributeEffectAspect" || ty == "WeaponAttributeEffectAspect") for (auto& ef : asp.value("attributeeffects", json::array())) {
            if (!ef.is_object() || !ef.value("AttributeToModify", json()).is_string()) continue;
            json src = value_source(ef.value("modifiervalue", json::object())); if (!src.is_object()) continue;
            static const char* mt[] = { "scale add", "pre add", "add", "multiply", "set" };
            const int m = (int)ef.value("ModifierType", 0.0);
            effects.push_back({ {"attribute", def_name_in(ef["AttributeToModify"].get<std::string>())}, {"how", m >= 0 && m < 5 ? mt[m] : "?"}, {"source", src} });
        }
    }
    return ok({ {"perks", perks}, {"effects", effects} });
}
// <data>\value_overrides.json [{kind: table, table, row, column, value, orig} | {kind: def, name, store, field, value, orig}].
// Only in a solo session; if the session stops being solo, the original values go back (nothing changed online).
static json g_ovr = json::array(); static bool g_ovr_loaded = false, g_ovr_applied = false;
static std::string ovr_file() { return g_data_dir + "\\value_overrides.json"; }
static void ovr_load() { if (g_ovr_loaded) return; g_ovr_loaded = true; try { std::ifstream f(ovr_file()); if (f) g_ovr = json::parse(f); } catch (...) {} if (!g_ovr.is_array()) g_ovr = json::array(); }
static void ovr_save() { std::ofstream(ovr_file()) << g_ovr.dump(1); }
static json def_cmd(const std::string& name, const json& a);
static json ovr_write(const json& e, const json& v) {   // one entry -> the game (menu_ok: the caller checked solo)
    json args = e; args["value"] = v; args["menu_ok"] = true;
    if (e.value("kind", std::string()) == "table") return def_cmd("TQ.TABLE_SET", args);
    json r = def_cmd("TQ.DEF_SET", args); return r;
}
static bool ovr_same(const json& a, const json& b) {
    if (a.value("kind", "") != b.value("kind", "")) return false;
    if (a.value("kind", "") == "table") return lower(a.value("table", "")) == lower(b.value("table", "")) && lower(a.value("row", "")) == lower(b.value("row", "")) && lower(a.value("column", "")) == lower(b.value("column", ""));
    return lower(a.value("name", "")) == lower(b.value("name", "")) && lower(a.value("field", "")) == lower(b.value("field", "")) && lower(a.value("store", "")) == lower(b.value("store", ""));
}
static void ovr_tick() {   // game thread, once a second
    static ULONGLONG at = 0; const ULONGLONG now = GetTickCount64(); if (now - at < 1000) return; at = now;
    ovr_load(); if (g_ovr.empty()) return;
    const bool solo = session().value("standalone", false) && player_pawn();
    if (solo && !g_ovr_applied) { int n = 0; for (auto& e : g_ovr) if (ovr_write(e, e["value"]).value("ok", false)) ++n;
        g_ovr_applied = true; plog("value overrides applied: " + std::to_string(n) + " of " + std::to_string(g_ovr.size()));
        if (n) toast("Your value edits are active (" + std::to_string(n) + ")"); }
    else if (!solo && g_ovr_applied && !player_pawn()) { /* main menu between games: keep them (no online session yet) */ }
    else if (!session().value("standalone", true) && g_ovr_applied) {   // a non-solo session: originals back
        for (auto& e : g_ovr) if (e.contains("orig")) ovr_write(e, e["orig"]);
        g_ovr_applied = false; plog("value overrides taken off (not a solo session)"); }
}
static void ovr_keeper() {   // keeper: only once there are overrides and a solo game (indexes the defs on first use)
    ovr_load(); if (g_ovr.empty()) return;
    if (g_def_idx.empty()) { if (!session().value("standalone", false)) return; def_index_build(); }
    ovr_tick();
}
static json ovr_cmd(const std::string& name, const json& a) {
    ovr_load();
    if (name == "TQ.VALUES") return ok({ {"overrides", g_ovr}, {"active", g_ovr_applied} });
    if (name == "TQ.VALUE_SET") {   // {kind: table, table, row, column | kind: def, name, store?, field, value}
        json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
        json e = json::object(); for (const char* k : { "kind", "table", "row", "column", "name", "store", "field" }) if (a.contains(k)) e[k] = a[k];
        if (!e.contains("kind")) e["kind"] = e.contains("table") ? "table" : "def";
        if (!a.contains("value")) return fail("BAD_ARG", "value");
        json r = ovr_write(e, a["value"]); if (!r.value("ok", false)) return r;
        json orig = r["data"].value("before", json());
        auto it = std::find_if(g_ovr.begin(), g_ovr.end(), [&](const json& x) { return ovr_same(x, e); });
        if (it == g_ovr.end()) { e["orig"] = orig; e["value"] = a["value"]; g_ovr.push_back(e); }
        else (*it)["value"] = a["value"];   // the first original stays
        g_ovr_applied = true; ovr_save();
        return r;
    }
    if (name == "TQ.VALUE_RESET") {   // {index} | {all: true} | the same keys as VALUE_SET: original back, edit forgotten
        int n = 0;
        for (size_t i = 0; i < g_ovr.size();) {
            const bool hit = a.value("all", false) || (a.contains("index") && a["index"].is_number() && (size_t)a["index"].get<int>() == i) || (!a.contains("index") && ovr_same(g_ovr[i], a));
            if (!hit) { ++i; continue; }
            if (g_ovr[i].contains("orig")) ovr_write(g_ovr[i], g_ovr[i]["orig"]);
            g_ovr.erase(g_ovr.begin() + i); ++n;
            if (a.contains("index")) break;
        }
        ovr_save(); return ok({ {"reset", n} });
    }
    return fail("UNKNOWN", name);
}
static json def_cmd(const std::string& name, const json& a) {
    if (g_def_idx.empty() || a.value("refresh", false)) def_index_build();
    if (g_def_idx.empty()) return fail("NO_DEFS", "no def stores found");
    if (name == "TQ.PART_INFO") {   // {addr}: a part's perks (card text + the number behind each placeholder) and stat effects
        uintptr_t at = 0; try { at = std::stoull(a.value("addr", std::string("0")), nullptr, 16); } catch (...) {}
        if (!at) return fail("BAD_ARG", "addr (part def address from itemdb.json)");
        return part_info(at);
    }
    if (name.rfind("TQ.TABLE_", 0) == 0) return table_cmd(name, a, [](const std::string& tn) -> uintptr_t {
        auto it = g_def_idx.find(lower(tn)); if (it == g_def_idx.end()) return 0;
        for (auto& r : it->second) if (store_kind(r.store) == "GbxUEDataTableDefs") return r.addr;
        return 0; });
    auto pick = [&](std::vector<DefRec>& out, std::string& err) -> bool {   // {name, store?}
        const std::string nm = lower(a.value("name", std::string())); if (nm.empty()) { err = "name"; return false; }
        auto it = g_def_idx.find(nm); if (it == g_def_idx.end()) { err = "no def named " + a.value("name", std::string()) + " (BL.DEF_FIND {text})"; return false; }
        const std::string sf = lower(a.value("store", std::string()));
        for (auto& r : it->second) if (sf.empty() || lower(store_kind(r.store)).find(sf) != std::string::npos) out.push_back(r);
        if (out.empty()) { err = "no def of that name in a store matching " + sf; return false; }
        return true;
    };
    auto brief = [](const std::string&, const DefRec& r) { uintptr_t st = def_struct(r.addr);
        return json{ {"name", r.name}, {"addr", hexstr(r.addr)}, {"store", store_kind(r.store)}, {"struct", st ? ue::name_of(st) : std::string("?")} }; };
    if (name == "TQ.DEF_STORES") {   // {filter}
        const std::string f = lower(a.value("filter", std::string())); json out = json::array();
        for (auto& [s, n] : g_def_stores) { const std::string k = store_kind(s); if (!f.empty() && lower(k).find(f) == std::string::npos) continue; out.push_back({ {"store", k}, {"defs", n}, {"addr", hexstr(s)} }); }
        return ok({ {"stores", out}, {"total_defs", g_def_total} });
    }
    if (name == "TQ.DEF_FIND" || name == "TQ.DEF_LIST") {   // {text | filter, store, max}
        const std::string q = lower(a.value("text", a.value("filter", std::string()))), sf = lower(a.value("store", std::string()));
        const size_t mx = a.value("max", 100); json out = json::array(); size_t total = 0;
        for (auto& [nm, recs] : g_def_idx) { if (!q.empty() && nm.find(q) == std::string::npos) continue;
            for (auto& r : recs) { if (!sf.empty() && lower(store_kind(r.store)).find(sf) == std::string::npos) continue;
                ++total; if (out.size() < mx) out.push_back({ {"name", r.name}, {"addr", hexstr(r.addr)}, {"store", store_kind(r.store)} }); } }
        return ok({ {"count", total}, {"defs", out} });
    }
    if (name == "TQ.DEF_GET") {   // {name, store?, field?, depth?}
        std::vector<DefRec> rs; std::string err; if (!pick(rs, err)) return fail("NOT_FOUND", err);
        if (rs.size() > 1 && a.value("store", std::string()).empty()) { json c = json::array(); for (auto& r : rs) c.push_back(brief(a.value("name", std::string()), r));
            return fail("AMBIGUOUS", std::to_string(rs.size()) + " defs have that name - add store: " + c.dump()); }
        const DefRec r = rs[0]; const uintptr_t st = def_struct(r.addr); json out = brief(a.value("name", std::string()), r);
        if (!st) return ok(out, "struct type unknown - fields not readable");
        const std::string fld = a.value("field", std::string());
        if (fld.empty()) out["fields"] = ue::struct_json(r.addr, st, 0, a.value("depth", 4));
        else { uintptr_t b = 0; ue::Prop leaf; if (!ue::resolve(r.addr, st, fld, b, leaf)) return fail("NO_FIELD", fld);
               out["field"] = fld; out["type"] = leaf.type; out["value"] = ue::get_num(b, leaf);
               if (leaf.type == "GbxDefPtrProperty") out["value"] = ue::def_text(b + leaf.offset);
               else if (leaf.type == "NameProperty") out["value"] = ue::name_str(ue::get<ue::FName>(b, leaf));
               else if (leaf.type == "StrProperty") out["value"] = ue::fstring(b + leaf.offset);
               else if (leaf.type == "TextProperty") out["value"] = ue::ftext(b + leaf.offset);
               else if (leaf.type == "StructProperty" && leaf.owner_struct) out["value"] = ue::struct_json(b + leaf.offset, leaf.owner_struct, 0);
               else if (leaf.type == "GbxInlineStructProperty") { uintptr_t inst = 0, ist = 0;   // polymorphic value: its kind + fields
                   if (ue::inline_target(b + leaf.offset, inst, ist)) { json v = ue::struct_json(inst, ist, 0); v["@type"] = ue::name_of(ist); out["value"] = v; } else out["value"] = nullptr; } }
        return ok(out);
    }
    if (name == "TQ.DEF_SET") {   // {name, store?, field, value}: changes the def in memory (until the game restarts)
        json s = session(); if (!s.value("standalone", false) && !a.value("menu_ok", false)) return fail("NOT_SOLO", "solo only");
        std::vector<DefRec> rs; std::string err; if (!pick(rs, err)) return fail("NOT_FOUND", err);
        if (rs.size() > 1 && a.value("store", std::string()).empty()) return fail("AMBIGUOUS", "several defs have that name - add store");
        const uintptr_t st = def_struct(rs[0].addr); if (!st) return fail("NO_STRUCT", "struct type unknown");
        if (!a.contains("value")) return fail("BAD_ARG", "value");
        json r = ue::field_set(rs[0].addr, st, a.value("field", std::string()), a["value"]);
        plog("def set " + a.value("name", std::string()) + "." + a.value("field", std::string()) + " -> " + a["value"].dump());
        return r;
    }
    return fail("UNKNOWN", name);
}

// Every GbxSerialNumberAwareDef (InventoryTypeDef, InventoryCompDef, InventoryPartDef ...) has SerialIndex at +0x18:
// {Category FName @+0x20, scope @+0x28, status @+0x29, Index int16 @+0x2A}; the serial stores Index.
static json defidx_cmd(const json& a) {
    json out = json::object();
    const std::string type = a.value("type", std::string("InventoryPartDef"));
    for (auto& n : a.value("names", json::array())) {
        const std::string nm = n.get<std::string>();
        uint8_t b[0x18]; std::string why;
        if (!ue::def_bytes("(\"" + nm + "\",\"" + type + "\")", b, &why)) { out[nm] = { {"err", why} }; continue; }
        json e = { {"bytes", to_hex(b, 0x18)} };
        for (int off = 0; off < 0x18; off += 8) {
            uintptr_t p = 0; memcpy(&p, b + off, 8);
            if (p < 0x10000 || p > 0x7FFFFFFFFFFF) continue;
            int16_t idx = 0; ue::FName cat{}; uint8_t sc = 0, st = 0;
            if (ue::rd_as(p + 0x2A, idx) && ue::rd_as(p + 0x20, cat) && ue::rd_as(p + 0x28, sc) && ue::rd_as(p + 0x29, st))
                e["at_" + std::to_string(off)] = { {"index", idx}, {"category", ue::name_str(cat)}, {"scope", sc}, {"status", st} };
        }
        out[nm] = e;
    }
    return ok(out);
}

// ------------------------------------------------------------------ item serial numbers (copy / export)

// GbxItem.data (GbxItemData) = {InstanceId u32, Identity InventoryIdentity @+8 (0xD8, not reflected)}.
// Identity + 0xA0 = char* serial ("@Ug..." base85, the code save editors and players share), +0xB0 = length.
// Backpack + equipped: OakPlayerState.BackpackItems.items[] (entry.InventoryItem.Item = GbxItem, .EquipSlot);
// bank: BankItems.items[]; ground: InventoryPickup.Item.
static std::string item_serial(uintptr_t gbx_item) {
    uintptr_t p = 0; uint64_t len = 0;
    if (!ue::rd_as(gbx_item + 8 + 0xA0, p) || !ue::rd_as(gbx_item + 8 + 0xB0, len) || !p || len == 0 || len > 512) return "";
    std::string s(len, '\0'); if (!ue::mread(p, s.data(), len)) return "";
    const size_t z = s.find('\0'); if (z != std::string::npos) s.resize(z);   // length counts the terminator
    for (char c : s) if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return "";
    return s;
}
static bool g_serials_parts = false;

// identity -> {type, comp, parts[{index, category, gestalt[], desc}]}  (InventoryPartDef: SerialIndex @+0x18, GestaltPartNames @+0x68,
// DebugDisplayDescription @+0x88; identity +0 type def (name @+0x58), +8 comp def (name @+0x48), +0x10 TArray<part def*>)
static json identity_parts(uintptr_t id) {
    json out = json::object();
    auto sidx = [](uintptr_t d) -> json { int16_t i = 0; ue::FName c{}; ue::rd_as(d + 0x2A, i); ue::rd_as(d + 0x20, c); return { {"index", i}, {"category", ue::name_str(c)} }; };
    uintptr_t t = 0, c = 0; ue::rd_as(id, t); ue::rd_as(id + 8, c);
    if (t) { out["type"] = sidx(t); out["type"]["name"] = def_name_at(t, 0x58); }
    if (c) { out["comp"] = sidx(c); out["comp"]["name"] = def_name_at(c, 0x48); }
    uintptr_t arr = 0; int32_t n = 0; ue::rd_as(id + 0x10, arr); ue::rd_as(id + 0x18, n);
    json parts = json::array();
    for (int32_t i = 0; i < n && i < 64 && arr; ++i) {
        uintptr_t d = 0; ue::rd_as(arr + i * 8, d); if (!d) continue;
        json pj = sidx(d); pj["addr"] = hexstr(d);
        uintptr_t gp = 0; int32_t gn = 0; ue::rd_as(d + 0x68, gp); ue::rd_as(d + 0x70, gn);
        json g = json::array(); for (int32_t k = 0; k < gn && k < 16 && gp; ++k) { ue::FName f{}; ue::rd_as(gp + k * 8, f); g.push_back(ue::name_str(f)); }
        pj["gestalt"] = g;
        uintptr_t sp = 0; int32_t sl = 0; ue::rd_as(d + 0x88, sp); ue::rd_as(d + 0x90, sl);
        if (sp && sl > 1 && sl < 256) { std::wstring w(sl - 1, L'\0'); if (ue::mread(sp, w.data(), (sl - 1) * 2)) pj["desc"] = std::string(w.begin(), w.end()); }
        uintptr_t back = 0; ue::rd_as(d + 0x30, back); if (back && back != t) pj["owner_type"] = def_name_at(back, 0x58);
        parts.push_back(pj);
    }
    out["parts"] = parts;
    return out;
}
static json container_serials(uintptr_t ps, const char* field) {
    json out = json::array();
    uintptr_t base = 0; ue::Prop arr; if (!ue::resolve(ps, ue::class_of(ps), std::string(field) + ".items", base, arr)) return out;
    ue::Prop inner = ue::array_inner(arr); if (!inner || !inner.owner_struct) return out;
    ue::Prop inv = ue::find_prop(inner.owner_struct, "InventoryItem"); if (!inv || !inv.owner_struct) return out;
    ue::Prop item = ue::find_prop(inv.owner_struct, "Item"), eq = ue::find_prop(inv.owner_struct, "EquipSlot");
    if (!item) return out;
    auto a = ue::array_of(base, arr);
    for (int32_t i = 0; i < a.num && i < 1000; ++i) {
        const uintptr_t e = (uintptr_t)a.data + (uintptr_t)i * inner.size;
        const uintptr_t gi = e + inv.offset + item.offset;
        json j = { {"slot", i}, {"serial", item_serial(gi)}, {"identity", hexstr(gi + 8)} };
        if (g_serials_parts && !j["serial"].get<std::string>().empty()) j["parts"] = identity_parts(gi + 8);
        if (eq) { const double es = ue::get_num(e + inv.offset, eq); if (es >= 0 && es < 250) j["equip_slot"] = es; }
        out.push_back(j);
    }
    return out;
}
static bool to_clipboard(const std::string& s) {
    if (!OpenClipboard(nullptr)) return false; EmptyClipboard();
    HGLOBAL h = GlobalAlloc(GMEM_MOVEABLE, s.size() + 1); if (!h) { CloseClipboard(); return false; }
    memcpy(GlobalLock(h), s.c_str(), s.size() + 1); GlobalUnlock(h); SetClipboardData(CF_TEXT, h); CloseClipboard(); return true;
}

// ------------------------------------------------------------------ item database (for the serial builder / editor)
// Every InventoryTypeDef / InventoryCompDef / InventoryPartDef lives in the game's static def data. A live item gives one
// vtable of each kind; a sweep of readable memory for those vtables finds all of them. Parts point at their type (+0x30).
// Output bin\itemdb.json: types {index: {name, maker, parts[{index, gestalt[], desc}]}}, comps [{index, name}], shared parts.
static json itemdb_cmd(const json& a) {
    const uintptr_t ps = player_ps(); if (!ps) return fail("NO_PS", "no player");
    // vtables from the backpack: type (+0), comp (+8), part (+0x10 array)
    uintptr_t vt_type = 0, vt_comp = 0, vt_part = 0, near_def = 0;
    json bp = container_serials(ps, "BackpackItems");
    for (auto& e : bp) {
        if (e.value("serial", std::string()).empty()) continue;
        const uintptr_t id = std::stoull(e["identity"].get<std::string>(), nullptr, 16);
        uintptr_t t = 0, c = 0, arr = 0; int32_t n = 0; ue::rd_as(id, t); ue::rd_as(id + 8, c); ue::rd_as(id + 0x10, arr); ue::rd_as(id + 0x18, n);
        if (t && !vt_type) ue::rd_as(t, vt_type);
        if (c && !vt_comp) ue::rd_as(c, vt_comp);
        if (arr && n > 0 && !vt_part) { uintptr_t d = 0; ue::rd_as(arr, d); if (d) { ue::rd_as(d, vt_part); near_def = d; } }
    }
    if (!vt_type || !vt_comp || !vt_part) return fail("NO_ITEMS", "need at least one weapon in the backpack to learn the def layouts");
    std::vector<uintptr_t> types, comps, parts;
    // the defs sit in one static-data arena: only sweep +-768 MB around a known part def
    const uintptr_t lo = near_def > (768ull << 20) ? near_def - (768ull << 20) : 0x10000, hi = near_def + (768ull << 20);
    MEMORY_BASIC_INFORMATION mbi; uintptr_t addr = lo; size_t scanned = 0;
    std::vector<uint8_t> buf;
    while (VirtualQuery((LPCVOID)addr, &mbi, sizeof mbi) == sizeof mbi) {
        const uintptr_t base = (uintptr_t)mbi.BaseAddress, end = base + mbi.RegionSize;
        const DWORD pr = mbi.Protect & 0xFF;
        if (mbi.State == MEM_COMMIT && !(mbi.Protect & PAGE_GUARD) && (pr == PAGE_READWRITE || pr == PAGE_READONLY || pr == PAGE_WRITECOPY) && mbi.RegionSize < (512u << 20)) {
            buf.resize(mbi.RegionSize);
            if (ue::mread(base, buf.data(), buf.size())) {
                scanned += buf.size();
                for (size_t o = 0; o + 8 <= buf.size(); o += 8) {
                    uintptr_t q; memcpy(&q, buf.data() + o, 8);
                    if (q == vt_part) parts.push_back(base + o); else if (q == vt_type) types.push_back(base + o); else if (q == vt_comp) comps.push_back(base + o);
                }
            }
        }
        if (end <= addr || end >= hi) break; addr = end;
    }
    auto idx = [](uintptr_t d) { int16_t i = -1; ue::rd_as(d + 0x2A, i); return (int)i; };
    auto fnames = [](uintptr_t arrp) { json g = json::array(); uintptr_t p = 0; int32_t n = 0; ue::rd_as(arrp, p); ue::rd_as(arrp + 8, n);
        for (int32_t k = 0; k < n && k < 16 && p; ++k) { ue::FName f{}; ue::rd_as(p + k * 8, f); g.push_back(ue::name_str(f)); } return g; };
    auto fstr = [](uintptr_t at) { uintptr_t sp = 0; int32_t sl = 0; ue::rd_as(at, sp); ue::rd_as(at + 8, sl); std::string o;
        if (sp && sl > 1 && sl < 256) { std::wstring w(sl - 1, L'\0'); if (ue::mread(sp, w.data(), (sl - 1) * 2)) o.assign(w.begin(), w.end()); } return o; };
    json T = json::object(); std::map<uintptr_t, std::string> tkey;
    for (uintptr_t t : types) { const int i = idx(t); const std::string k = std::to_string(i) + "@" + hexstr(t);
        tkey[t] = k; T[k] = { {"index", i}, {"name", def_name_at(t, 0x58)}, {"addr", hexstr(t)}, {"parts", json::array()} }; }
    json C = json::array();
    for (uintptr_t c : comps) { json cj = { {"index", idx(c)}, {"base", def_name_at(c, 0x48)}, {"rarity", def_name_at(c, 0x60)}, {"addr", hexstr(c)} };
        const std::string nm = fstr(c + 0x38); if (!nm.empty()) cj["item"] = nm; C.push_back(cj); }
    int orphan = 0;
    for (uintptr_t p : parts) {
        uintptr_t owner = 0; ue::rd_as(p + 0x30, owner);
        json pj = { {"index", idx(p)}, {"gestalt", fnames(p + 0x68)}, {"addr", hexstr(p)} };
        { auto clean = [](const std::string& x) { if (x.empty() || x == "None" || x.size() > 96) return false; for (unsigned char c : x) if (c < 0x20 || c > 0x7E) return false; return true; };
          const std::string sl = def_name_at(p, 0x40); if (clean(sl)) pj["slot"] = sl;   // barrel / magazine / payload ...
          // the part's real name lives in its first Aspect (+0x58 array, element +0x10): "JAK_AR.part_mag_ted_thrown_01",
          // "Weapon.part_corrosive", "grenade_gadget.part_01_mirv", "...part_firmware_airstrike"
          uintptr_t asp = 0, e0 = 0; int32_t an = 0; ue::rd_as(p + 0x58, asp); ue::rd_as(p + 0x60, an);
          if (asp && an > 0 && an < 64 && ue::rd_as(asp, e0) && e0) { const std::string nm = def_name_at(e0, 0x10); if (clean(nm)) pj["name"] = nm; } }
        const std::string d = fstr(p + 0x88); if (!d.empty()) pj["desc"] = d;
        auto it = tkey.find(owner);
        if (it == tkey.end()) { ++orphan; continue; }
        T[it->second]["parts"].push_back(pj);
    }
    // maker from the most common gestalt prefix (DAD_SM_Body -> DAD)
    for (auto& [k, t] : T.items()) {
        std::map<std::string, int> cnt;
        for (auto& p : t["parts"]) for (auto& g : p["gestalt"]) { std::string s = g.get<std::string>(); const size_t u = s.find('_'); if (u != std::string::npos && u <= 4) cnt[s.substr(0, u)]++; }
        std::string best; int bn = 0; for (auto& [m, n] : cnt) if (n > bn) { best = m; bn = n; }
        t["maker"] = best;
    }
    json out = { {"types", T}, {"comps", C} };
    const std::string path = g_cfg.root + "\\itemdb.json";
    { const std::string txt = out.dump(1, ' ', false, json::error_handler_t::replace); std::ofstream f(path + ".tmp"); f << txt; }
    MoveFileExA((path + ".tmp").c_str(), path.c_str(), MOVEFILE_REPLACE_EXISTING);
    return ok({ {"file", path}, {"types", types.size()}, {"comps", comps.size()}, {"parts", parts.size()}, {"orphan_parts", orphan}, {"scanned_mb", scanned >> 20} });
}

static json serials_cmd(const json& a) {
    g_serials_parts = a.value("parts", false);
    uintptr_t ps = player_ps(); if (!ps) return fail("NO_PS", "no player state");
    const std::string where = a.value("where", std::string("backpack"));
    json out = json::object();
    if (where == "backpack" || where == "all" || where == "equipped") {
        json b = container_serials(ps, "BackpackItems");
        if (where == "equipped") { json e = json::array(); for (auto& x : b) if (x.contains("equip_slot")) e.push_back(x); b = e; }
        out["backpack"] = b;
    }
    if (where == "bank" || where == "all") out["bank"] = container_serials(ps, "BankItems");
    if (where == "ground" || where == "all") {
        json g = json::array();
        for (uintptr_t x : all_pickups()) { uintptr_t b = 0; ue::Prop leaf; if (ue::resolve(x, ue::class_of(x), "Item", b, leaf)) g.push_back({ {"addr", hexstr(x)}, {"serial", item_serial(b + leaf.offset)} }); }
        out["ground"] = g;
    }
    // export: <project>\serials.txt (append, with a timestamp) + optional clipboard of one serial
    std::string txt; for (auto& kv : out.items()) for (auto& x : kv.value()) if (!x.value("serial", "").empty()) txt += kv.key() + "\t" + x.value("serial", "") + "\n";
    if (a.value("save", true) && !txt.empty()) {
        const std::string proj = ::wb::g_cfg.root;   // the trainer folder (api.json sits right in it: its parent is the user's folder)
        SYSTEMTIME st; GetLocalTime(&st); char ts[64]; sprintf_s(ts, "# %04d-%02d-%02d %02d:%02d:%02d\n", st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond);
        std::ofstream(proj + "\\serials.txt", std::ios::app) << ts << txt;
        out["file"] = proj + "\\serials.txt";
    }
    if (a.contains("copy")) {   // {copy: "equipped:0"} = weapon slot 1, or a backpack slot number
        const std::string c = a["copy"].is_string() ? a["copy"].get<std::string>() : std::to_string(a["copy"].get<int>());
        std::string s;
        for (auto& x : container_serials(ps, "BackpackItems")) {
            if (c.rfind("equipped:", 0) == 0 ? (x.contains("equip_slot") && (int)x["equip_slot"].get<double>() == std::stoi(c.substr(9))) : x.value("slot", -1) == std::stoi(c)) { s = x.value("serial", ""); break; }
        }
        out["copied"] = !s.empty() && to_clipboard(s); out["serial"] = s;
    }
    return ok(out);
}

// ------------------------------------------------------------------ fly / no-clip (engine: movement mode + collision)
static json fly_cmd(const json& a) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    uintptr_t pawn = player_controlled(); if (!pawn) return fail("NO_PAWN", "no player character");
    const bool on = a.value("on", true), noclip = a.value("noclip", false);
    uintptr_t mv = prop_obj(pawn, "CharacterMovement"); if (!mv) return fail("NO_MOVEMENT", "no CharacterMovement");
    // EMovementMode: 1 walking, 3 falling, 5 flying
    json r = ue::call_json(mv, "SetMovementMode", { {"NewMovementMode", on ? 5 : 1}, {"NewCustomMode", 0} }, 0);
    json c = ue::call_json(pawn, "SetActorEnableCollision", { {"bNewActorEnableCollision", !(on && noclip)} }, 0);
    plog(std::string("fly ") + (on ? "on" : "off") + (noclip ? " noclip" : ""));
    return ok({ {"movement", r.value("ok", false)}, {"collision", c.value("ok", false)}, {"fly", on}, {"noclip", on && noclip} });
}

// ------------------------------------------------------------------ position / teleport (engine Actor functions)
static json player_pos() {
    uintptr_t pawn = player_controlled(); if (!pawn) return fail("NO_PAWN", "no player character");
    json r = ue::call_json(pawn, "K2_GetActorLocation", json::object(), pawn);
    if (!r.value("ok", false)) return r;
    return ok(r["data"]["ReturnValue"]);
}
static json teleport_to(double x, double y, double z) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    uintptr_t pawn = player_controlled(); if (!pawn) return fail("NO_PAWN", "no player character");
    json args = { {"NewLocation", { {"X", x}, {"Y", y}, {"Z", z} }}, {"bSweep", false}, {"bTeleport", true} };
    json r = ue::call_json(pawn, "K2_SetActorLocation", args, pawn);
    plog("teleport -> " + std::string(r.value("ok", false) ? "ok" : r.value("message", std::string("failed"))));
    return r;
}

static const json* spec_item(const std::string& id) {
    const json tabs = g_spec_json.value("tabs", json::array());
    for (auto& t : tabs) { const json secs = t.value("sections", json::array());
        for (auto& s : secs) { const json items = s.value("items", json::array());
            for (auto& it : items)
                if (it.value("id", "") == id) { static thread_local json keep; keep = it; return &keep; } } }
    return nullptr;
}
static void put_result(uint64_t seq, bool okv, const std::string& res) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const std::string k = std::to_string(seq);
    g_results[k] = { {"ok", okv}, {"res", res} };
    g_result_order.push_back(seq);
    while (g_result_order.size() > 20) { g_results.erase(std::to_string(g_result_order.front())); g_result_order.pop_front(); }
    if (seq > g_seq_done) g_seq_done = seq;
    ++g_ver;
}

static void hotkeys_tick();
static void turf_load();
void init(const std::string& data_dir, const std::string& panel_spec) {
    g_data_dir = data_dir; g_spec = panel_spec;
    const size_t k = panel_spec.find_last_of("\\/");
    g_api_path = (k == std::string::npos ? std::string(".") : panel_spec.substr(0, k)) + "\\api.json";
    load_api(true); load_spec();
    { json j = read_json(toggles_file()); if (j.is_object()) { std::lock_guard<std::mutex> lk(g_mtx); g_active = j; } }
    try { turf_load(); } catch (std::exception& e) { plog(std::string("territories load failed: ") + e.what()); }
    { json j = read_json(diff_file()); if (j.is_object()) { for (auto& kv : j.items()) if (g_diff_def.contains(kv.key())) {
            // only a value of the type the setting has (a wrong one made every later read throw)
            const json& d = g_diff_def[kv.key()]; const json& v = kv.value();
            if (d.is_boolean() ? v.is_boolean() : d.is_string() ? v.is_string() : v.is_number()) g_diff[kv.key()] = v; }
        g_diff_snap = g_diff; } }
    try { diff_live_load(); } catch (...) { g_dene.clear(); }
    { json j = read_json(luck_file());   // gear luck of a build that was running when the previous copy was replaced
      try { if (j.is_object() && j.contains("pid") && j["pid"].is_number() && j["pid"].get<uint64_t>() == (uint64_t)GetCurrentProcessId()) {
                g_luck_orphan.handle = hex_bytes(j.value("handle", std::string())); g_luck_orphan.pawn = std::stoull(j.value("pawn", std::string("0")), nullptr, 16); }
            else if (!j.is_null()) DeleteFileA(luck_file().c_str());
      } catch (...) { g_luck_orphan.handle.clear(); } }
    { json j = read_json(g_data_dir + "\\hud.json"); if (j.is_object()) for (const char* k : { "on", "enemies", "loot", "radius_m" }) if (j.contains(k) && !j[k].is_string()) hud_set(k, j[k]); }
    { json j = read_json(attrs_file()); if (j.is_object()) for (auto& kv : j.items()) {
        if (!kv.value().is_object()) continue;
        AttrMod m; m.name = kv.key();
        try { m.type = kv.value().value("type", 4); m.value = kv.value().value("value", 1.0); } catch (...) { continue; }
        const std::string hx = kv.value().is_object() && kv.value().contains("handle") && kv.value()["handle"].is_string() ? kv.value()["handle"].get<std::string>() : std::string();
        try { for (size_t i = 0; i + 1 < hx.size(); i += 3) m.handle.push_back((uint8_t)std::stoul(hx.substr(i, 2), nullptr, 16)); } catch (...) { m.handle.clear(); }
        try { m.pawn = std::stoull(kv.value().value("pawn", std::string("0")), nullptr, 16); } catch (...) { m.pawn = 0; }
        g_mods[m.name] = m; } }
    static bool ticking = false;
    if (!ticking) {
        ticking = true;
        add_main_tick([] {
            static ULONGLONG last = 0; const ULONGLONG now = GetTickCount64();
            if (now - last < 1000 || !job_on("file_watch")) return; last = now;
            PerfScope ps("file_watch");
            load_api(false);
            static FILETIME st{}; FILETIME t{}; if (ftime(g_spec, t) && CompareFileTime(&t, &st) != 0) { st = t; load_spec(); }
        });
        add_main_tick([] {
            if (!ue::ready()) {
                static ULONGLONG at = 0; const ULONGLONG now = GetTickCount64();
                if (now - at < 2000) return; at = now;
                if (!ue::init()) return;
            }
            { PerfScope ps("ALL keeper"); try { keeper_tick(); } catch (...) {} }
            if (job_on("hotkeys_tick")) { PerfScope ps("hotkeys_tick"); try { hotkeys_tick(); } catch (...) {} }
        });
    }
    ::wb::log("[bl4] engine ready: api %s, panel %s", g_api_path.c_str(), panel_spec.c_str());
}

bool native_mode() { return true; }

// ------------------------------------------------------------------ ENEMY CONTROL (BL.ENEMY_LIST / BL.ENEMY / BL.ENEMY_SPAWN / BL.SPAWNERS / BL.SPAWN_SWAP)
// GetTeamFromActorDef, the game's enemy table GetEnemiesActorDefInfo, kind from the name).
// Live enemy: OakCharacter Get/SetCharacterRank|EnemyType|Army (GameplayTags "GbxActor.Character.Rank.Badass.Ultimate"),
// GameStage (level, display only - stats are computed at spawn), team = GbxTeamFunctionLibrary::SetTeam(gdh:Team_X:15)
// (Team_Player = ally: attitude 0), size = SetActorScale3D, weapon = EquipAIInventoryItem(InventoryIdentity copied from a
// backpack item, 0xD8 bytes) + SwitchToAIWeapon, revive = GoFromDeadToHealthy, DropLoot, LaunchCharacter.
// Spawners: OakSpawnerComponent.GbxActorDef (GameDataHandle {FName, cat 16384}) = what that spot spawns - mostly
// "Mix_<area>_<faction>" spawn lists. The spawn swap rewrites it on every loaded enemy spawner (originals kept, "off" restores).
static json g_ecat;
static std::map<std::string, json> g_ecat_by;
static const json& enemy_cat() {
    if (g_ecat.is_null()) {
        const size_t k = g_api_path.find_last_of("\\/"); g_ecat = read_json(g_api_path.substr(0, k) + "\\enemies.json");
        if (!g_ecat.is_array()) g_ecat = json::array();
        for (auto& e : g_ecat) g_ecat_by[e.value("name", "")] = e;
    }
    return g_ecat;
}
static std::string strip_tag(std::string t, const char* pre) { const size_t n = strlen(pre); if (t.rfind(pre, 0) == 0) t = t.substr(n); return t == "None" ? "" : t; }
static std::string char_tag(uintptr_t c, const char* fn) {
    json r = ue::call_json(c, fn, json::object(), 0);
    return r.value("ok", false) ? r["data"]["ReturnValue"]["value"].value("TagName", std::string()) : std::string();
}
static double attr_n(const std::string& n, uintptr_t c) { json v = attr_get(n, c); return v.is_number() ? v.get<double>() : -1; }
static double prop_n(uintptr_t o, const char* f) { uintptr_t b = 0; ue::Prop leaf; return ue::resolve(o, ue::class_of(o), f, b, leaf) ? ue::get_num(b, leaf) : -1; }
static bool is_healthy(uintptr_t c) { json h = ue::call_json(c, "IsHealthy", json::object(), 0); return !(h.value("ok", false) && h["data"]["ReturnValue"].value("value", 1.0) == 0); }
static std::string faction_of(const std::string& team) {
    static const std::map<std::string, std::string> f = { {"Team_Bandit", "Rippers"}, {"Team_Order", "The Order"}, {"Team_Tediore", "Tediore"},
        {"Team_Dahl", "Dahl"}, {"Team_Corrupt", "Corrupted"}, {"Team_Pangolin", "Pangolins"}, {"Team_Creep", "Creeps"}, {"Team_Scav", "Scavengers"},
        {"Team_Loot", "Loot creatures"}, {"Team_Thresher", "Threshers"}, {"Team_Kraggon", "Kraggons"}, {"Team_Cat", "Cats"}, {"Team_Bat", "Bats"},
        {"Team_Beast", "Beasts"}, {"Team_Guardian", "Guardians"}, {"Team_NPC", "NPC"}, {"Team_Player", "YOUR SIDE"} };
    auto it = f.find(team); return it != f.end() ? it->second : strip_tag(team, "Team_");
}
static json enemy_info(uintptr_t c, const V3& p) {
    V3 v; actor_loc(c, v);
    const std::string nm = base_name(ue::name_of(c)), team = team_of(c);
    const std::string rank = strip_tag(char_tag(c, "GetCharacterRank"), "GbxActor.Character.Rank.");
    const double hp = attr_n("actor_current_total_health", c), mx = attr_n("actor_max_total_health", c);
    json j = { {"addr", hexstr(c)}, {"name", nm}, {"team", team}, {"faction", faction_of(team)}, {"rank", rank},
        {"type", strip_tag(char_tag(c, "GetCharacterEnemyType"), "GbxActor.Character.EnemyType.")},
        {"level", (int)prop_n(c, "GameStage")}, {"health", std::round(hp)}, {"max_health", std::round(mx)},
        {"pct", mx > 0 ? std::round(hp / mx * 100) : 0}, {"alive", is_healthy(c)}, {"ally", team == "Team_Player"},
        {"boss", rank.rfind("Boss", 0) == 0}, {"dist_m", std::round(std::sqrt((v.x - p.x) * (v.x - p.x) + (v.y - p.y) * (v.y - p.y) + (v.z - p.z) * (v.z - p.z)) / 10) / 10},
        {"pos", { {"X", v.x}, {"Y", v.y}, {"Z", v.z} }} };
    auto ce = g_ecat_by.find(nm); if (ce != g_ecat_by.end()) j["kind"] = ce->second.value("kind", "");
    return j;
}
// sliced = from the shared sliced character list with raw location reads and remembered teams (tick paths: no full object
// walk); false = a fresh full walk (one-off commands that need the exact list right now)
static std::vector<uintptr_t> enemy_chars(double radius_cm, bool allies, bool dead, bool sliced = false) {
    std::vector<uintptr_t> out; const uintptr_t pawn = player_pawn(); V3 p; if (!pawn || !actor_loc(pawn, p)) return out;
    if (sliced) g_char_scan.want();
    const std::vector<uintptr_t> src = sliced ? g_char_scan.live() : ue::find_all(ue::find_class("OakCharacter"), false, 3000);
    for (uintptr_t c : src) {
        if (c == pawn) continue; V3 v; if (!(sliced ? actor_loc_fast(c, v) : actor_loc(c, v))) continue;
        if ((v.x - p.x) * (v.x - p.x) + (v.y - p.y) * (v.y - p.y) + (v.z - p.z) * (v.z - p.z) > radius_cm * radius_cm) continue;
        if (base_name(ue::name_of(c)).rfind("Char_NPC", 0) == 0) continue;
        const std::string t = sliced ? team_cached(c) : team_of(c);
        if (t == "Team_Player" ? !allies : friendly_team(t)) continue;
        if (!dead && !is_healthy(c)) continue;
        out.push_back(c);
    }
    return out;
}
static json enemy_list_cmd(const json& a) {
    const uintptr_t pawn = player_pawn(); V3 p; if (!pawn || !actor_loc(pawn, p)) return fail("NO_PAWN", "no player character");
    enemy_cat();
    json out = json::array();
    for (uintptr_t c : enemy_chars(a.value("radius_m", 120.0) * 100, a.value("allies", true), a.value("dead", false))) out.push_back(enemy_info(c, p));
    std::sort(out.begin(), out.end(), [](const json& x, const json& y) { return x.value("dist_m", 0.0) < y.value("dist_m", 0.0); });
    if (out.size() > (size_t)a.value("max", 80)) out.erase(out.begin() + a.value("max", 80), out.end());
    return ok({ {"count", out.size()}, {"enemies", out} });
}
// target: "0x..." | "nearest" | "aim" (closest to your crosshair) | "all" (every enemy in radius_m) | "allies"
static std::vector<uintptr_t> enemy_targets(const json& a, bool dead) {
    std::vector<uintptr_t> out; const std::string t = a.value("target", std::string("nearest"));
    if (t.rfind("0x", 0) == 0) { const uintptr_t c = std::stoull(t, nullptr, 16); if (ue::alive(c)) out.push_back(c); return out; }
    const uintptr_t pawn = player_pawn(); V3 p, f; if (!pawn || !actor_loc(pawn, p) || !actor_fwd(pawn, f)) return out;
    auto list = enemy_chars(a.value("radius_m", t == "all" || t == "allies" ? 60.0 : 150.0) * 100, t == "allies", dead);
    if (t == "allies") { for (uintptr_t c : list) if (team_of(c) == "Team_Player") out.push_back(c); return out; }
    if (t == "all") { for (uintptr_t c : list) if (team_of(c) != "Team_Player") out.push_back(c); return out; }
    uintptr_t best = 0; double bs = 1e18;
    for (uintptr_t c : list) {
        V3 v; actor_loc(c, v); const double dx = v.x - p.x, dy = v.y - p.y, dz = v.z - p.z, d = std::sqrt(dx * dx + dy * dy + dz * dz) + 1;
        double s = d;
        if (t == "aim") { const double dot = (dx * f.x + dy * f.y + dz * f.z) / d; if (dot < 0.9) continue; s = (1 - dot) * 100000 + d * 0.05; }
        if (s < bs) { bs = s; best = c; }
    }
    if (best) out.push_back(best);
    return out;
}
static void diff_forget(uintptr_t c) {   // an enemy that became an ally: drop the difficulty modifiers, never copy it
    auto it = g_dene.find(c); if (it == g_dene.end()) { DiffEnemy e; e.ignore = true; g_dene[c] = e; return; }
    for (auto& h : it->second.h) remove_mod(h); it->second.h.clear(); it->second.ignore = true; g_dene_dirty = true;
}
static std::string backpack_identity_hex(int slot, std::string* label) {
    const uintptr_t ps = player_ps(); if (!ps) return "";
    json bp = container_serials(ps, "BackpackItems");
    if (slot < 0) {   // random weapon from the backpack
        std::vector<int> guns;
        for (auto& e : bp) { uintptr_t id = std::stoull(e.value("identity", std::string("0")), nullptr, 16), t = 0; ue::rd_as(id, t);
            const std::string tn = def_name_at(t, 0x58); if (tn.rfind("Weapon_", 0) == 0 || tn == "heavy_weapon_gadget") guns.push_back(e.value("slot", 0)); }
        if (guns.empty()) return ""; slot = guns[(size_t)(rnd01() * guns.size()) % guns.size()];
    }
    for (auto& e : bp) if (e.value("slot", -1) == slot) {
        const uintptr_t id = std::stoull(e.value("identity", std::string("0")), nullptr, 16); if (!id) return "";
        uint8_t buf[0xD8]; if (!ue::mread(id, buf, sizeof buf)) return "";
        if (label) { uintptr_t t = 0, c = 0; ue::rd_as(id, t); ue::rd_as(id + 8, c); *label = def_name_at(t, 0x58) + " " + def_name_at(c, 0x48); }
        static const char* hx = "0123456789ABCDEF"; std::string s; s.reserve(sizeof buf * 2);
        for (uint8_t b : buf) { s += hx[b >> 4]; s += hx[b & 15]; }
        return s;
    }
    return "";
}
static bool enemy_arm(uintptr_t c, int slot, std::string* label) {
    const std::string hx = backpack_identity_hex(slot, label); if (hx.empty()) return false;
    json r = ue::call_json(c, "EquipAIInventoryItem", { {"Identity", "hex:" + hx}, {"bReplaceExisting", true} }, 0);
    if (!r.value("ok", false)) return false;
    ue::call_json(c, "SwitchToAIWeapon", { {"WeaponSlotIndex", 0}, {"bInstantSwitch", true} }, 0);
    return true;
}
static bool set_team(uintptr_t c, const std::string& team) {
    static uintptr_t lib = 0; if (!lib || !ue::alive(lib)) lib = ue::cdo_of(ue::find_class("GbxTeamFunctionLibrary"));
    g_team_cache.erase(c);   // team_cached() asks the game again
    return ue::call_json(lib, "SetTeam", { {"AIActor", "addr:" + hexstr(c)}, {"Team", "gdh:" + team + ":15"} }, 0).value("ok", false);
}
static std::string original_team(uintptr_t c) {
    static uintptr_t lib = 0; if (!lib || !ue::alive(lib)) lib = ue::cdo_of(ue::find_class("GbxTeamFunctionLibrary"));
    json r = ue::call_json(lib, "GetTeamFromActorDef", { {"GbxActorDef", "gdh:" + base_name(ue::name_of(c)) + ":16384"} }, 0);
    return r.value("ok", false) ? r["data"]["ReturnValue"].value("value", std::string()) : std::string();
}
// one action on one enemy; v = the action's value
static bool enemy_act(uintptr_t c, const std::string& act, const json& v, std::string& msg) {
    auto num = [&](double d) { return v.is_number() ? v.get<double>() : v.is_string() ? atof(v.get<std::string>().c_str()) : d; };
    if (act == "kill") return ue::call_json(c, "Die", { {"type", 0} }, 0).value("ok", false);
    if (act == "freeze" || act == "unfreeze") { std::string w; return write_field(c, "CustomTimeDilation", act == "freeze" ? 0.0 : 1.0, &w); }
    if (act == "ally") { diff_forget(c); return set_team(c, "Team_Player"); }
    if (act == "enemy") { const std::string t = original_team(c); return !t.empty() && t != "Team_Player" && set_team(c, t); }
    if (act == "team") { std::string t = v.is_string() ? v.get<std::string>() : ""; if (t.rfind("Team_", 0) != 0) t = "Team_" + t; return set_team(c, t); }
    if (act == "rank") {
        static const std::map<std::string, std::string> r = { {"normal", "Normal"}, {"elite", "Elite"}, {"badass", "Badass"}, {"super", "Badass.Super"},
            {"ultimate", "Badass.Ultimate"}, {"mini", "Boss.Mini"}, {"boss", "Boss"}, {"raid", "Boss.Raid"}, {"chump", "Chump"} };
        const std::string k = lower(v.is_string() ? v.get<std::string>() : ""); auto it = r.find(k); if (it == r.end()) { msg = "rank: normal|elite|badass|super|ultimate|mini|boss|raid|chump"; return false; }
        return ue::call_json(c, "SetCharacterRank", { {"Rank", { {"TagName", "GbxActor.Character.Rank." + it->second} }} }, 0).value("ok", false);
    }
    if (act == "scale") { const double s = std::clamp(num(1), 0.1, 10.0); return ue::call_json(c, "SetActorScale3D", { {"NewScale3D", { {"X", s}, {"Y", s}, {"Z", s} }} }, 0).value("ok", false); }
    if (act == "health" || act == "damage" || act == "speed") {
        const double x = std::clamp(num(1), 0.01, 100.0); bool okv = true;
        if (act == "health") {
            for (int i = 1; i <= 3; ++i) { const std::string n = "ai_health_resource_0" + std::to_string(i) + "_max_value"; if (i == 1 || !attr_get(n, c).is_object()) okv &= add_mod(c, n, 3, x, nullptr); }
            static uintptr_t ds = 0; if (!ds || !ue::alive(ds)) ds = ue::cdo_of(ue::find_class("DamageStatics"));
            for (const char* t : { "HealthType_AI_Flesh", "HealthType_AI_Armor", "HealthType_AI_Energy", "HealthType_AI_Bone", "HealthType_AI_Corruption", "HealthType_AI_Eridium", "HealthType_AI_Ordonite" })
                ue::call_json(ds, "RefillHealthPercent", { {"Context", "addr:" + hexstr(c)}, {"HealthType", std::string("def:(\"") + t + "\",\"HealthTypeDef\")"}, {"Percent", 1.0}, {"MaxPercent", 1.0} }, 0);
            return okv;
        }
        return add_mod(c, act == "damage" ? "damage_dealt_multiplier" : "ground_speed_scale", 3, x, nullptr);
    }
    if (act == "weapon") { std::string l; const bool r = enemy_arm(c, v.is_number() ? v.get<int>() : -1, &l); msg = l; return r; }
    if (act == "bring") {
        const uintptr_t pawn = player_pawn(); V3 p, f; if (!actor_loc(pawn, p) || !actor_fwd(pawn, f)) return false;
        const double ang = rnd01() * 1.2 - 0.6, d = 600;
        const V3 at{ p.x + (f.x * std::cos(ang) - f.y * std::sin(ang)) * d, p.y + (f.y * std::cos(ang) + f.x * std::sin(ang)) * d, p.z + 50 };
        return ue::call_json(c, "K2_SetActorLocation", { {"NewLocation", { {"X", at.x}, {"Y", at.y}, {"Z", at.z} }}, {"bSweep", false}, {"bTeleport", true} }, 0).value("ok", false);
    }
    if (act == "goto") { V3 e; if (!actor_loc(c, e)) return false; return teleport_to(e.x + 300, e.y, e.z + 150).value("ok", false); }
    if (act == "revive") return ue::call_json(c, "GoFromDeadToHealthy", json::object(), 0).value("ok", false);
    if (act == "loot") return ue::call_json(c, "DropLoot", json::object(), 0).value("ok", false);
    if (act == "launch") return ue::call_json(c, "LaunchCharacter", { {"LaunchVelocity", { {"X", 0.0}, {"Y", 0.0}, {"Z", num(2500)} }}, {"bXYOverride", false}, {"bZOverride", true} }, 0).value("ok", false);
    msg = "unknown action " + act; return false;
}
static json enemy_cmd(const json& a) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    const std::string act = a.value("action", std::string());
    if (act.empty()) return fail("BAD_ARG", "action: kill|freeze|unfreeze|ally|enemy|team|rank|scale|health|damage|speed|weapon|bring|goto|revive|loot|launch");
    auto list = enemy_targets(a, act == "revive");
    if (list.empty()) return fail("NO_TARGET", "no enemy matches target '" + a.value("target", std::string("nearest")) + "'");
    if (act == "goto") list.resize(1);
    int n = 0; std::string msg; json done = json::array();
    for (uintptr_t c : list) { if (enemy_act(c, act, a.value("v", json()), msg)) { ++n; done.push_back(base_name(ue::name_of(c))); } }
    plog("enemy " + act + " x" + std::to_string(n) + (msg.empty() ? "" : " (" + msg + ")"));
    if (!n) return fail("FAILED", msg.empty() ? act + " did not work on " + base_name(ue::name_of(list[0])) : msg);
    return ok({ {"action", act}, {"done", n}, {"enemies", done}, {"note", msg} });
}

// spawn with options: the spawn is latent, so new characters of that def are caught by enemy_tick and get the options
struct SpawnJob { std::string def; json opts; std::set<uintptr_t> before; int left = 0; ULONGLONG until = 0; };
static std::vector<SpawnJob> g_spawn_jobs;
static json enemy_spawn_cmd(const json& a) {
    json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
    std::string def = a.value("name", std::string());
    if (def.empty() || def == "random_boss") { auto b = boss_list(); if (b.empty()) return fail("NO_BOSS", "no boss list"); def = b[(size_t)(rnd01() * b.size()) % b.size()]; }
    else def = actor_for(def);
    if (def.empty()) return fail("NO_ACTOR", "no character matches '" + a.value("name", std::string()) + "'");
    const uintptr_t pawn = player_pawn(); V3 p, f; if (!pawn || !actor_loc(pawn, p) || !actor_fwd(pawn, f)) return fail("NO_PAWN", "no player character");
    const int count = std::clamp(a.value("count", 1), 1, 20); const double dist = a.value("dist", 900.0);
    SpawnJob j; j.def = def; j.opts = a; j.until = GetTickCount64() + 8000;
    for (uintptr_t c : ue::find_all(ue::find_class("OakCharacter"), false, 3000)) j.before.insert(c);
    int n = 0;
    for (int i = 0; i < count; ++i) {
        const double side = (i - (count - 1) / 2.0) * 260.0;
        const V3 at{ p.x + f.x * dist - f.y * side, p.y + f.y * dist + f.x * side, p.z + 60 };
        if (spawn_char_at(def, at)) { ++n; expect_spawn(def, false); }   // the difficulty tweak treats them as copies: no extra friends
    }
    if (!n) return fail("SPAWN", "the game did not spawn " + def);
    j.left = n; g_spawn_jobs.push_back(j);
    plog("enemy spawn " + def + " x" + std::to_string(n));
    return ok({ {"actor", def}, {"spawned", n} });
}

// world spawn swap
static std::map<uintptr_t, ue::FName> g_spawner_orig;
static json g_swap = { {"mode", "off"}, {"v", ""}, {"replaced", 0} };
static int g_spawner_def_off = -1;
static bool spawner_def_off(uintptr_t s) {
    if (g_spawner_def_off >= 0) return true;
    ue::Prop pr = ue::find_prop(ue::class_of(s), "GbxActorDef"); if (!pr) return false; g_spawner_def_off = pr.offset; return true;
}
static std::string spawner_def(uintptr_t s) { if (!spawner_def_off(s)) return ""; ue::FName n{}; if (!ue::rd_as(s + g_spawner_def_off, n)) return ""; return ue::name_str(n); }
static bool spawner_write(uintptr_t s, const ue::FName& n) {   // the game's own SpawnerComponent::SetGbxActorDef (keeps its caches right)
    if (!ue::alive(s)) return false;
    return ue::call_json(s, "SetGbxActorDef", { {"NewGbxActorDef", "gdh:" + ue::name_str(n) + ":16384"}, {"bDestroyExistingActors", false} }, 0).value("ok", false);
}
static bool enemy_spawner(const std::string& d) {
    if (d.rfind("Mix_", 0) != 0 && d.rfind("Char_", 0) != 0) return false;
    const std::string l = lower(d); return l.find("npc") == std::string::npos && l.find("town") == std::string::npos && l.find("vendor") == std::string::npos;
}
static bool swap_good_pick(const std::string& n) {   // things that make sense standing in the world (no cinematic actors, clones, parts)
    const std::string l = lower(n);
    for (const char* bad : { "cin_", "clone", "antenna", "reinforcement", "attach", "_shared", "child", "prototype", "hologram", "npc", "bossadd", "plot", "mission" })
        if (l.find(bad) != std::string::npos) return false;
    return true;
}
static const char* kWarTeams[] = { "Team_Bandit", "Team_Order", "Team_Tediore", "Team_Dahl", "Team_Corrupt", "Team_Scav",
    "Team_Creep", "Team_Pangolin", "Team_Thresher", "Team_Kraggon", "Team_Cat", "Team_Bat", "Team_Beast" };
static uint32_t g_territory_seed = 1;
static std::string territory_team(double x, double y) {   // the world cut into 400 m squares, each owned by one faction
    const int cx = (int)std::floor(x / 40000.0), cy = (int)std::floor(y / 40000.0);
    uint32_t h = (uint32_t)cx * 73856093u ^ (uint32_t)cy * 19349663u ^ g_territory_seed * 83492791u; h ^= h >> 13; h *= 0x5bd1e995u; h ^= h >> 15;
    const bool people = g_swap.value("v", std::string()) != "all";
    const int n = people ? 6 : 13; return kWarTeams[h % n];
}
static std::string swap_pick(const std::string& orig, const std::string& team = "", const V3* at = nullptr) {
    const std::string mode = g_swap.value("mode", std::string("off")), v = g_swap.value("v", std::string());
    std::vector<std::string> pool;
    auto from_cat = [&](auto pred) { for (auto& e : enemy_cat()) if (e.value("enemy_table", false) && pred(e) && swap_good_pick(e.value("name", ""))) pool.push_back(e.value("name", "")); };
    if (mode == "def") return actor_for(v);
    if (mode == "bosses") { pool = boss_list(); }
    else if (mode == "badass") {
        const std::string lo = lower(orig);
        if (!team.empty()) from_cat([&](const json& e) { return e.value("kind", "") == "badass" && e.value("team", "") == team; });
        if (pool.empty())
        from_cat([&](const json& e) { if (e.value("kind", "") != "badass") return false; const std::string tm = lower(e.value("team", "")); const std::string f = tm.size() > 5 ? tm.substr(5) : ""; return !f.empty() && lo.find(f) != std::string::npos; });
        if (pool.empty()) from_cat([](const json& e) { return e.value("kind", "") == "badass"; });
    }
    else if (mode == "territory" && at) { const std::string t = territory_team(at->x, at->y);
        from_cat([&](const json& e) { const std::string k = e.value("kind", ""); return e.value("team", "") == t && (k == "regular" || k == "badass"); }); }
    else if (mode == "war") {   // every enemy a random faction: they fight each other
        const std::string t = kWarTeams[(size_t)(rnd01() * 13) % 13];
        from_cat([&](const json& e) { const std::string k = e.value("kind", ""); return e.value("team", "") == t && (k == "regular" || k == "badass"); }); }
    else if (mode == "faction") { std::string t = v.rfind("Team_", 0) == 0 ? v : "Team_" + v;
        from_cat([&](const json& e) { const std::string k = e.value("kind", ""); return e.value("team", "") == t && (k == "regular" || k == "badass"); }); }
    else if (mode == "chaos") from_cat([](const json& e) { const std::string k = e.value("kind", ""); return k == "regular" || k == "badass" || k == "loot creature"; });
    if (pool.empty()) return "";
    return pool[(size_t)(rnd01() * pool.size()) % pool.size()];
}
static void swap_tick() {
    const std::string mode = g_swap.value("mode", std::string("off"));
    if (mode == "off" && g_spawner_orig.empty()) return;
    const auto all = ue::find_all(ue::find_class("OakSpawnerComponent"), false, 4000);
    if (mode != "off") { std::set<uintptr_t> live(all.begin(), all.end()); for (auto i = g_spawner_orig.begin(); i != g_spawner_orig.end();) i = live.count(i->first) ? std::next(i) : g_spawner_orig.erase(i); }
    for (uintptr_t s : all) {
        auto it = g_spawner_orig.find(s);
        if (mode == "off") { if (it != g_spawner_orig.end()) spawner_write(s, it->second); continue; }
        if (it != g_spawner_orig.end()) continue;
        const std::string d = spawner_def(s); if (!enemy_spawner(d)) continue;
        if (prop_n(s, "SpawnDetails.bCritical") > 0) continue;   // mission-critical spawn: never touched
        const std::string nd = swap_pick(d); ue::FName fn{}; if (nd.empty() || !ue::name_find(nd, fn)) continue;
        ue::FName old{}; ue::rd_as(s + g_spawner_def_off, old);
        if (spawner_write(s, fn)) { g_spawner_orig[s] = old; g_swap["swapped"] = g_swap.value("swapped", 0) + 1; }
    }
    if (mode == "off") { g_spawner_orig.clear(); g_swap["swapped"] = 0; }
}
static std::set<uintptr_t> g_swap_seen;

// ------------------------------------------------------------------ TERRITORIES (learned from the game, shuffled, conquered)
// in native Nexus config). So the trainer LEARNS the real map: every naturally spawned hostile tells which faction lives in
// its 200 m square. Saved in <data>\territories.json and grown as you play. On top of it:
//   shuffle  - the game's borders, new owners (each faction's turf goes to another faction)
//   conquest - squares have an owner (native at first); raid squads of a neighbouring faction attack camps; if the raiders
//              wipe out the defenders the square changes owner for good and its spawns become the winner's.
struct Turf { std::map<std::string, int> seen; std::string owner; ULONGLONG taken = 0; std::string from; };
static std::map<int64_t, Turf> g_turf;
static bool g_turf_dirty = false;
static std::map<std::string, std::string> g_shuffle;      // team -> team
static json g_turf_log = json::array();                  // recent conquest events
static double g_raid_minutes = 3.0;
static const double kTurfCell = 20000.0;                  // 200 m
static int64_t turf_key(double x, double y) { const int32_t cx = (int32_t)std::floor(x / kTurfCell), cy = (int32_t)std::floor(y / kTurfCell); return ((int64_t)cx << 32) ^ (uint32_t)cy; }
static bool war_team(const std::string& t) { for (const char* w : kWarTeams) if (t == w) return true; return false; }
static std::string turf_native(const Turf& t) { std::string b; int n = 0; for (auto& [k, v] : t.seen) if (v > n) { n = v; b = k; } return b; }
static std::string turf_owner(int64_t k) { auto it = g_turf.find(k); if (it == g_turf.end()) return ""; return it->second.owner.empty() ? turf_native(it->second) : it->second.owner; }
static std::string turf_file() { return g_data_dir + "\\territories.json"; }
static void turf_save() {
    json j = json::object();
    for (auto& [k, t] : g_turf) { json e = { {"seen", t.seen} }; if (!t.owner.empty()) { e["owner"] = t.owner; e["from"] = t.from; } j[std::to_string(k)] = e; }
    std::ofstream(turf_file()) << json({ {"cells", j}, {"shuffle", g_shuffle}, {"log", g_turf_log}, {"raid_minutes", g_raid_minutes} }).dump();
    g_turf_dirty = false;
}
static void turf_load() {
    json j = read_json(turf_file()); if (!j.is_object()) return;
    const json cells = j.value("cells", json::object());   // keep it alive: items() of a temporary dangles
    for (auto& kv : cells.items()) {
        const json seen = kv.value().value("seen", json::object());
        Turf t; for (auto& s2 : seen.items()) t.seen[s2.key()] = s2.value().get<int>();
        t.owner = kv.value().value("owner", std::string()); t.from = kv.value().value("from", std::string());
        g_turf[std::stoll(kv.key())] = t;
    }
    const json sh = j.value("shuffle", json::object());
    for (auto& kv : sh.items()) g_shuffle[kv.key()] = kv.value().get<std::string>();
    g_turf_log = j.value("log", json::array()); g_raid_minutes = j.value("raid_minutes", 3.0);
    plog("territories loaded: " + std::to_string(g_turf.size()) + " squares");
}
static void turf_learn(const V3& v, const std::string& team) {
    if (!war_team(team)) return;
    Turf& t = g_turf[turf_key(v.x, v.y)]; ++t.seen[team]; g_turf_dirty = true;
}
static void shuffle_new() {   // people factions swap among themselves, creatures among themselves
    std::vector<std::string> a(kWarTeams, kWarTeams + 6), b(kWarTeams + 6, kWarTeams + 13);
    g_shuffle.clear();
    for (auto* grp : { &a, &b }) {
        std::vector<std::string> to = *grp;
        for (int tries = 0; tries < 50; ++tries) {   // a derangement: nobody keeps its own turf
            for (size_t i = to.size() - 1; i > 0; --i) std::swap(to[i], to[(size_t)(rnd01() * (i + 1)) % (i + 1)]);
            bool ok2 = true; for (size_t i = 0; i < to.size(); ++i) if (to[i] == (*grp)[i]) ok2 = false;
            if (ok2) break;
        }
        for (size_t i = 0; i < to.size(); ++i) g_shuffle[(*grp)[i]] = to[i];
    }
    g_turf_dirty = true;
}

// pick_from_list rolls a unit the way the list does: an entry with a condition counts only when its tier is unlocked
// (Attr_Infamy_MixVariety*_<None|Low|Med|High>: rank 0..3 <= tier) and then weighs if_weight; sub-lists are rolled again.
static json g_lists;
static const json& spawn_lists() {
    if (g_lists.is_null()) { const size_t k = g_api_path.find_last_of("\\/"); json j = read_json(g_api_path.substr(0, k) + "\\spawnlists.json"); g_lists = j.is_object() ? j.value("lists", json::object()) : json::object(); }
    return g_lists;
}
static int tier_rank(const std::string& cond) {
    if (cond.size() > 5 && cond.compare(cond.size() - 5, 5, "_None") == 0) return 0;
    if (cond.size() > 4 && cond.compare(cond.size() - 4, 4, "_Low") == 0) return 1;
    if (cond.size() > 4 && cond.compare(cond.size() - 4, 4, "_Med") == 0) return 2;
    if (cond.size() > 5 && cond.compare(cond.size() - 5, 5, "_High") == 0) return 3;
    return 0;   // other conditions (badass limit ...) count as always on
}
static std::string pick_from_entries(const json& entries, int tier, int depth = 0) {
    if (!entries.is_array() || entries.empty() || depth > 5) return "";
    std::vector<std::pair<const json*, double>> w; double tot = 0;
    for (auto& e : entries) {
        double x = e.value("weight", 1.0);
        if (e.contains("if")) { if (tier_rank(e.value("if", std::string())) > tier) continue; x = e.value("if_weight", x); }
        if (x <= 0) continue;
        const std::string d = e.value("def", std::string());
        if (d.rfind("Char_", 0) == 0 && !swap_good_pick(d)) continue;
        w.push_back({ &e, x }); tot += x;
    }
    if (tot <= 0) return "";
    double r = rnd01() * tot;
    for (auto& [e, x] : w) { if ((r -= x) <= 0) {
        if (e->contains("list")) return pick_from_entries((*e)["list"], tier, depth + 1);
        return e->value("def", std::string()); } }
    return w.back().first->value("def", std::string());
}
static std::string pick_from_list(const std::string& mix, int tier) {
    const json& L = spawn_lists(); auto it = L.find(mix); if (it == L.end()) return "";
    for (int t = 0; t < 4; ++t) { const std::string d = pick_from_entries(*it, tier); if (!d.empty()) return d; }
    return "";
}
static std::string spawner_mix_of(uintptr_t c) {   // the Mix_ list of the spawner that made this enemy ("" = not from a spawner)
    static uintptr_t lib = 0; if (!lib || !ue::alive(lib)) lib = ue::cdo_of(ue::find_class("SpawnBlueprintLibrary"));
    json r = ue::call_json(lib, "GetSpawnerComponent", { {"Context", "addr:" + hexstr(c)} }, 0);
    if (!r.value("ok", false)) return "";
    const std::string a = r["data"]["ReturnValue"].value("addr", std::string("0x0"));
    const uintptr_t sc = std::stoull(a, nullptr, 16); if (!sc) return "";
    const std::string d = spawner_def(sc); return d.rfind("Mix_", 0) == 0 ? d : "";
}
// faction keyword in list names
static std::string team_word(const std::string& team) {
    static const std::map<std::string, std::string> w = { {"Team_Bandit", "Bandit"}, {"Team_Order", "Order"}, {"Team_Tediore", "Tediore"},
        {"Team_Dahl", "Dahl"}, {"Team_Corrupt", "Corrupt"}, {"Team_Scav", "Scav"}, {"Team_Creep", "Creep"}, {"Team_Pangolin", "Pangolin"},
        {"Team_Thresher", "Thresher"}, {"Team_Kraggon", "Kraggon"}, {"Team_Cat", "Cat"}, {"Team_Bat", "Bat"}, {"Team_Beast", "Beast"} };
    auto it = w.find(team); return it == w.end() ? "" : it->second;
}
// a real list of `team` for the region of `mix` (Mix_GRA_... -> GRA), else any list of that faction
static std::string regional_list(const std::string& mix, const std::string& team) {
    const std::string word = team_word(team); if (word.empty()) return "";
    std::string region; { const size_t a = mix.find('_'), b = a == std::string::npos ? a : mix.find('_', a + 1); if (b != std::string::npos) region = mix.substr(a + 1, b - a - 1); }
    std::vector<std::string> best, any;
    for (auto& kv : spawn_lists().items()) {
        const std::string& n = kv.key(); const std::string l = lower(n);
        if (lower(n).find(lower(word)) == std::string::npos) continue;
        if (l.find("boss") != std::string::npos || l.find("test") != std::string::npos || l.find("intro") != std::string::npos || l.find("npc") != std::string::npos) continue;
        any.push_back(n); if (!region.empty() && n.rfind("Mix_" + region + "_", 0) == 0) best.push_back(n);
    }
    const auto& v = best.empty() ? any : best; if (v.empty()) return "";
    return v[(size_t)(rnd01() * v.size()) % v.size()];
}

static std::string team_of_def(const std::string& def) { enemy_cat(); auto it = g_ecat_by.find(def); return it == g_ecat_by.end() ? "" : it->second.value("team", ""); }
static std::string team_unit(const std::string& team, bool badass) {
    std::vector<std::string> pool;
    for (auto& e : enemy_cat()) {
        if (!e.value("enemy_table", false) || e.value("team", "") != team || !swap_good_pick(e.value("name", ""))) continue;
        const std::string k = e.value("kind", "");
        if (badass ? k == "badass" : k == "regular") pool.push_back(e.value("name", ""));
    }
    if (pool.empty() && badass) return team_unit(team, false);
    return pool.empty() ? "" : pool[(size_t)(rnd01() * pool.size()) % pool.size()];
}
// who should stand here? "" = leave the enemy as it is
static std::string turf_target_team(const std::string& mode, const std::string& team, const V3& v) {
    if (!war_team(team)) return "";
    if (mode == "shuffle") { auto it = g_shuffle.find(team); return it == g_shuffle.end() ? "" : it->second; }
    if (mode == "conquest") { auto it = g_turf.find(turf_key(v.x, v.y)); if (it == g_turf.end() || it->second.owner.empty()) return ""; return it->second.owner != team ? it->second.owner : ""; }
    return "";
}
// conquest raids
struct Raid { std::string attacker, defender; int64_t cell = 0; V3 camp; std::vector<uintptr_t> raiders; ULONGLONG started = 0; int expect = 0; std::set<uintptr_t> before; };
static std::vector<Raid> g_raids;
static ULONGLONG g_raid_next = 0;
static void turf_event(const std::string& text) {
    g_turf_log.push_back({ {"t", (int64_t)time(nullptr)}, {"text", text} }); while (g_turf_log.size() > 30) g_turf_log.erase(g_turf_log.begin());
    toast(text); plog("territory: " + text); g_turf_dirty = true;
}
static std::string raid_start(const V3& p) {
    // a camp = at least 3 living hostiles of one war faction close together, 60..200 m from you
    std::map<std::string, std::vector<std::pair<uintptr_t, V3>>> by;
    for (uintptr_t c : enemy_chars(25000, false, false)) { const std::string t = team_of(c); if (!war_team(t)) continue; V3 v; if (actor_loc(c, v)) by[t].push_back({ c, v }); }
    if (by.empty()) return "no enemies within 250 m to raid";
    for (auto& [team, list] : by) {
        for (auto& [c0, v0] : list) {
            int grp = 0; for (auto& x : list) if (std::hypot(x.second.x - v0.x, x.second.y - v0.y) < 4000) ++grp;
            const double dp = std::hypot(v0.x - p.x, v0.y - p.y);
            if (grp < 3 || dp < 4000 || dp > 25000) continue;
            const int64_t cell = turf_key(v0.x, v0.y);
            // attacker: the owner of a neighbouring square that is not this faction, else another faction of the same kind
            std::string att;
            for (int dx = -1; dx <= 1 && att.empty(); ++dx) for (int dy = -1; dy <= 1 && att.empty(); ++dy) {
                const std::string o = turf_owner(turf_key(v0.x + dx * kTurfCell, v0.y + dy * kTurfCell)); if (!o.empty() && o != team && war_team(o)) att = o; }
            if (att.empty()) { const bool people = std::find(kWarTeams, kWarTeams + 6, team) != kWarTeams + 6; const int lo = people ? 0 : 6, n = people ? 6 : 7;
                for (int k = 0; k < 10 && (att.empty() || att == team); ++k) att = kWarTeams[lo + (size_t)(rnd01() * n) % n]; }
            if (att.empty() || att == team) continue;
            Raid r; r.attacker = att; r.defender = team; r.cell = cell; r.camp = v0; r.started = GetTickCount64();
            for (uintptr_t c : ue::find_all(ue::find_class("OakCharacter"), false, 3000)) r.before.insert(c);
            // the squad lands on the far side of the camp (away from you), 30..45 m out
            const double ang = std::atan2(v0.y - p.y, v0.x - p.x);
            const int n = 3 + (int)(rnd01() * 3);
            for (int i = 0; i < n; ++i) {
                std::string def; { const std::string rl = regional_list(spawner_mix_of(c0), att); if (!rl.empty()) def = pick_from_list(rl, 3); }
                if (def.empty() || team_of_def(def) != att) def = team_unit(att, rnd01() < 0.3);
                if (def.empty()) continue;
                const double a = ang + (rnd01() - 0.5) * 0.9, d = 3000 + rnd01() * 1500;
                if (spawn_char_at(def, V3{ v0.x + std::cos(a) * d, v0.y + std::sin(a) * d, v0.z + 80 })) { ++r.expect; expect_spawn(def, true); }
            }
            if (!r.expect) continue;
            turf_event(faction_of(att) + " are raiding a " + faction_of(team) + " camp!");
            g_raids.push_back(r); return "";
        }
    }
    return "no camp (3+ of one faction together, 40-250 m away) found";
}
static void raid_tick(const V3& p) {
    const ULONGLONG now = GetTickCount64();
    if (g_raids.empty()) return;
    // one sliced character list + one hostile list for all raids (it was two full object walks per raid every second)
    g_char_scan.want();
    const std::vector<uintptr_t> chars = g_char_scan.live();
    const std::vector<uintptr_t> foes = enemy_chars(1e9, false, false, true);
    for (auto& r : g_raids) {   // pick up the raid squad as it appears
        if ((int)r.raiders.size() >= r.expect) continue;
        for (uintptr_t c : chars) {
            if (r.before.count(c)) continue; r.before.insert(c);
            if (team_cached(c) == r.attacker) { V3 v; if (actor_loc_fast(c, v) && std::hypot(v.x - r.camp.x, v.y - r.camp.y) < 8000) r.raiders.push_back(c); }
        }
    }
    for (auto it = g_raids.begin(); it != g_raids.end();) {
        Raid& r = *it;
        int att_alive = 0, def_alive = 0;
        for (uintptr_t c : r.raiders) if (ue::alive(c) && is_healthy(c)) ++att_alive;
        for (uintptr_t c : foes) { if (team_cached(c) != r.defender) continue; V3 v; if (actor_loc_fast(c, v) && std::hypot(v.x - r.camp.x, v.y - r.camp.y) < 6000) ++def_alive; }
        const bool squad_in = (int)r.raiders.size() >= r.expect || now - r.started > 15000;
        bool done = false;
        if (squad_in && !r.raiders.empty() && att_alive == 0) { turf_event(faction_of(r.defender) + " held their ground against " + faction_of(r.attacker) + "."); done = true; }
        else if (squad_in && def_alive == 0 && att_alive > 0) {
            Turf& t = g_turf[r.cell]; t.from = turf_owner(r.cell); t.owner = r.attacker; t.taken = now;
            turf_event(faction_of(r.attacker) + " took this turf from " + faction_of(r.defender) + "!"); done = true;
        } else if (now - r.started > 300000 || std::hypot(p.x - r.camp.x, p.y - r.camp.y) > 40000) done = true;   // 5 min or you left: called off
        if (done) g_raid_next = now + (ULONGLONG)(g_raid_minutes * 60000);   // a breather before the next raid
        it = done ? g_raids.erase(it) : std::next(it);
    }
}
static json turf_here() {
    const uintptr_t pawn = player_pawn(); V3 p; if (!pawn || !actor_loc(pawn, p)) return json();
    const int64_t k = turf_key(p.x, p.y); auto it = g_turf.find(k);
    json o = { {"cells_known", g_turf.size()} };
    if (it == g_turf.end()) { o["native"] = "unknown yet"; o["owner"] = "unknown yet"; return o; }
    const std::string nat = turf_native(it->second); o["native"] = faction_of(nat);
    const std::string mode = g_swap.value("mode", std::string("off"));
    std::string now_owner = nat;
    if (mode == "shuffle") { auto s2 = g_shuffle.find(nat); if (s2 != g_shuffle.end()) now_owner = s2->second; }
    if (mode == "conquest" && !it->second.owner.empty()) now_owner = it->second.owner;
    o["owner"] = faction_of(now_owner);
    int owned = 0; for (auto& [kk, t] : g_turf) if (!t.owner.empty() && t.owner != turf_native(t)) ++owned; o["conquered_cells"] = owned;
    return o;
}

static json territory_here();
static json swap_cmd(const json& a) {
    if (a.contains("mode")) {
        json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
        const std::string m = a.value("mode", std::string("off"));
        if (m != "off" && m != "def" && m != "bosses" && m != "badass" && m != "faction" && m != "chaos" && m != "territory" && m != "war" && m != "shuffle" && m != "conquest" && m != "roster")
            return fail("BAD_ARG", "mode: off|def|bosses|badass|faction|territory|war|chaos|shuffle|conquest");
        if (m == "shuffle" && (g_shuffle.empty() || a.value("reshuffle", true))) shuffle_new();
        if (m == "conquest") {   // squares start with their native owner (conquered ones keep their conqueror)
            for (auto& [k, t] : g_turf) if (t.owner.empty()) t.owner = turf_native(t);
            g_raid_next = 0; g_raids.clear(); g_turf_dirty = true; }
        if (m == "territory") g_territory_seed = (uint32_t)(rnd01() * 4e9) + 1;   // new map of territories every time
        // a new mode re-rolls every spawner: restore the originals first
        g_swap["mode"] = m; g_swap["v"] = a.value("v", std::string()); g_swap["replaced"] = 0;
        std::ofstream(g_data_dir + "\\spawnswap.json") << g_swap.dump(1);
        plog("spawn swap " + m + " " + g_swap.value("v", std::string()));
    }
    if (a.contains("raid_minutes") && a["raid_minutes"].is_number()) { g_raid_minutes = std::clamp(a["raid_minutes"].get<double>(), 0.5, 60.0); g_turf_dirty = true; }
    if (a.value("raid_now", false)) { json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");   // it spawns enemies
        const uintptr_t pawn = player_pawn(); V3 p; if (pawn && actor_loc(pawn, p)) { const std::string why = raid_start(p); if (!why.empty()) return fail("NO_RAID", why); } }
    if (a.value("reset_conquest", false)) { for (auto& [k, t] : g_turf) { t.owner = g_swap.value("mode", std::string()) == "conquest" ? turf_native(t) : ""; t.from.clear(); } g_turf_log = json::array(); g_turf_dirty = true; }
    if (g_turf_dirty) turf_save();
    json o = g_swap; if (o.value("mode", std::string()) == "territory") o["territory"] = territory_here();
    o["turf"] = turf_here(); o["log"] = g_turf_log; o["raids"] = (int)g_raids.size(); o["raid_minutes"] = g_raid_minutes;
    if (!g_shuffle.empty()) { json sh = json::object(); for (auto& [k, v] : g_shuffle) sh[faction_of(k)] = faction_of(v); o["shuffle"] = sh; }
    return ok(o);
}
static json territory_here() {
    const uintptr_t pawn = player_pawn(); V3 p; if (!pawn || !actor_loc(pawn, p)) return json();
    return { {"here", faction_of(territory_team(p.x, p.y))}, {"north", faction_of(territory_team(p.x + 40000, p.y))}, {"south", faction_of(territory_team(p.x - 40000, p.y))},
             {"east", faction_of(territory_team(p.x, p.y + 40000))}, {"west", faction_of(territory_team(p.x, p.y - 40000))} };
}
static json spawners_cmd(const json& a) {
    const uintptr_t pawn = player_pawn(); V3 p; if (!pawn || !actor_loc(pawn, p)) return fail("NO_PAWN", "no player character");
    const double r = a.value("radius_m", 400.0) * 100; json out = json::array(); std::map<std::string, int> kinds;
    for (uintptr_t s : ue::find_all(ue::find_class("OakSpawnerComponent"), false, 4000)) {
        json l = ue::call_json(s, "K2_GetComponentLocation", json::object(), 0); if (!l.value("ok", false)) continue;
        const json& v = l["data"]["ReturnValue"]["value"]; const double dx = v.value("X", 0.0) - p.x, dy = v.value("Y", 0.0) - p.y, d = std::sqrt(dx * dx + dy * dy);
        const std::string def = spawner_def(s); ++kinds[def];
        if (d > r) continue;
        json e = { {"addr", hexstr(s)}, {"def", def}, {"dist_m", std::round(d / 100)}, {"critical", prop_n(s, "SpawnDetails.bCritical") > 0},
                   {"respawn_s", prop_n(s, "SpawnDetails.RespawnPeriodOverride")} };
        auto it = g_spawner_orig.find(s); if (it != g_spawner_orig.end()) e["original"] = ue::name_str(it->second);
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const json& x, const json& y) { return x.value("dist_m", 0.0) < y.value("dist_m", 0.0); });
    return ok({ {"loaded", kinds}, {"near", out}, {"swap", g_swap} });
}
// replace enemies as they appear (far from you, so the world simply looks that way when you get there)
static bool swap_replaceable(const std::string& n) {
    const std::string l = lower(n);
    for (const char* bad : { "boss", "plot", "mission", "side", "bounty", "quest", "named", "unique", "npc", "_shared", "turret", "attach", "clone", "child" })
        if (l.find(bad) != std::string::npos) return false;
    auto it = g_ecat_by.find(n); if (it == g_ecat_by.end()) return false;
    const std::string k = it->second.value("kind", ""); return k == "regular" || k == "badass";
}

// The character list comes from the shared sliced scan g_char_scan (SliceScan, top of the file; stepped by the keeper).
static void swap_replace_tick() {
    const std::string mode = g_swap.value("mode", std::string("off"));
    // every mode reads the shared sliced character list (no full object walk 4x a second): off = slow slices, just to
    // learn the map once per completed pass; a swap mode = a slice every tick
    static uint64_t learn_pass = 0;
    g_char_scan.want(mode != "off");
    if (mode == "off") { if (g_char_scan.passes == learn_pass) return; learn_pass = g_char_scan.passes; }
    enemy_cat();
    const uintptr_t pawn = player_pawn(); V3 p; if (!pawn || !actor_loc(pawn, p)) return;
    static ULONGLONG save_at = 0; if (g_turf_dirty && GetTickCount64() - save_at > 30000) { save_at = GetTickCount64(); turf_save(); }
    static V3 last{}; static uintptr_t last_pawn = 0; static ULONGLONG calm_at = 0; const ULONGLONG now = GetTickCount64();
    const double jump = (p.x - last.x) * (p.x - last.x) + (p.y - last.y) * (p.y - last.y);
    if (pawn != last_pawn || jump > 3000.0 * 3000.0) calm_at = now + 5000;   // teleport / travel / new pawn: let the world finish streaming
    last = p; last_pawn = pawn;
    if (now < calm_at) return;
    if (!g_char_scan.passes) return;   // first pass still running
    const std::vector<uintptr_t> chars = g_char_scan.live();   // dead / reused addresses already dropped
    {   static ULONGLONG prune_at = 0;   // characters that are gone leave the "seen" set: a new one at the same address is new
        if (now - prune_at > 5000) { prune_at = now; for (auto i = g_swap_seen.begin(); i != g_swap_seen.end();) i = ue::alive(*i) ? std::next(i) : g_swap_seen.erase(i); } }
    for (uintptr_t c : chars) {
        if (g_swap_seen.size() > 30000) g_swap_seen.clear();   // safety net only (pruned above)
        if (c == pawn || !g_swap_seen.insert(c).second) continue;
        if (prop_n(c, "bActorIsBeingDestroyed") > 0) continue;
        const std::string n = base_name(ue::name_of(c));
        auto ex = g_swap_expect.find(n);
        if (ex == g_swap_expect.end()) for (auto x = g_swap_expect.begin(); x != g_swap_expect.end(); ++x) if (n.rfind(x->first + "_", 0) == 0) { ex = x; break; }
        if (ex != g_swap_expect.end()) { if (--ex->second <= 0) g_swap_expect.erase(ex); continue; }   // one of ours
        V3 v; if (!actor_loc_fast(c, v)) continue;
        { const std::string t0 = team_cached(c); turf_learn(v, t0); }   // the real map, as the game spawns it
        if (mode == "off") continue;
        if (!swap_replaceable(n)) continue;
        if ((v.x - p.x) * (v.x - p.x) + (v.y - p.y) * (v.y - p.y) + (v.z - p.z) * (v.z - p.z) < 4000.0 * 4000.0) continue;   // in plain view: leave it
        const std::string tm = team_cached(c);
        if (friendly_team(tm) || !is_healthy(c)) continue;
        std::string def;
        if (mode == "roster") {   // the enemy's own spawn list, rolled with every tier unlocked (max infamy)
            const std::string mix = spawner_mix_of(c); if (mix.empty()) continue;
            def = pick_from_list(mix, g_swap.value("v", std::string()) == "med" ? 2 : 3);
        } else if (mode == "shuffle" || mode == "conquest") {
            const std::string tt = turf_target_team(mode, tm, v); if (tt.empty()) continue;
            const std::string rl = regional_list(spawner_mix_of(c), tt);
            def = rl.empty() ? "" : pick_from_list(rl, 3);
            if (def.empty() || team_of_def(def) != tt) def = team_unit(tt, lower(n).find("badass") != std::string::npos);
        } else def = swap_pick(n, tm, &v);
        if (def.empty() || def == n) continue;
        // NOT K2_DestroyActor: spawners keep pointers to their actors -> two heap-corruption crashes. The game's own death
        // path (no loot, body hidden) keeps the spawner's bookkeeping right.
        diff_forget(c);
        ue::call_json(c, "SuppressDropOnDeathItemPools", { {"bSuppress", true}, {"LootPoolTypeToSuppress", 0} }, 0);   // ELootPoolTypes::All
        if (!ue::call_json(c, "Die", { {"type", 0} }, 0).value("ok", false)) continue;
        ue::call_json(c, "SetActorHiddenInGame", { {"bNewHidden", true} }, 0);
        ue::call_json(c, "SetActorEnableCollision", { {"bNewActorEnableCollision", false} }, 0);
        if (spawn_char_at(def, V3{ v.x, v.y, v.z + 40 })) {
            expect_spawn(def, true);
            g_swap["replaced"] = g_swap.value("replaced", 0) + 1;
        }
    }
}
static void enemy_tick() {
    static ULONGLONG at = 0, sat = 0, xat = 0; const ULONGLONG now = GetTickCount64();
    if (now - xat > 1000) { xat = now; expect_expire(); }
    // Everything below except learning the map (swap mode "off": reads only) changes the game: checked again while it runs,
    // not only when it was switched on (a friend joining later must stop the kills / spawns / raids).
    const bool writes = !g_spawn_jobs.empty() || g_swap.value("mode", std::string("off")) != "off";
    if (writes) {
        static ULONGLONG solo_at = 0; static bool solo = false;
        if (now - solo_at > 3000) { solo_at = now; solo = session().value("standalone", false); }
        if (!solo) return;
    }
    if (!g_spawn_jobs.empty() && now - at > 250) {
        at = now;
        g_char_scan.want();
        auto all = g_char_scan.live();   // sliced list (no full object walk 4x a second while a spawn is pending)
        for (auto& j : g_spawn_jobs) {
            for (uintptr_t c : all) {
                if (j.left <= 0 || j.before.count(c)) continue;
                { const std::string bn = base_name(ue::name_of(c)); if (bn != j.def && bn.rfind(j.def + "_", 0) != 0) continue; }
                j.before.insert(c); --j.left; std::string m;
                const json& o = j.opts;
                if (o.value("ally", false)) enemy_act(c, "ally", json(), m);
                for (const char* k : { "rank", "scale", "health", "damage", "speed" }) if (o.contains(k) && !o[k].is_null() && o[k] != "" && o[k] != 1 && o[k] != 1.0) enemy_act(c, k, o[k], m);
                if (o.contains("weapon") && o["weapon"].is_number() && o["weapon"].get<int>() >= -1) enemy_act(c, "weapon", o["weapon"], m);
            }
        }
        g_spawn_jobs.erase(std::remove_if(g_spawn_jobs.begin(), g_spawn_jobs.end(), [&](const SpawnJob& j) { return j.left <= 0 || now > j.until; }), g_spawn_jobs.end());
    }
    // spawners stream in with the world and spawn soon after: check 4x a second so they are swapped before they spawn
    // NOTE: rewriting spawners (SetGbxActorDef on every loaded spawner) crashed the game during a teleport (heap corruption
    // while level streaming unloaded them) and the game resets the def on activation anyway -> only enemies are replaced.
    if (now - sat > 250) { sat = now; try { swap_replace_tick(); } catch (...) {} }
    if (g_swap.value("mode", std::string()) == "conquest") {
        static ULONGLONG rat = 0;
        if (now - rat > 1000) { rat = now; try {
            const uintptr_t pawn = player_pawn(); V3 p;
            if (pawn && actor_loc(pawn, p)) {
                raid_tick(p);
                if (!g_raid_next) g_raid_next = now + 60000;   // first raid a minute after turning it on
                if (g_raids.empty() && now >= g_raid_next) { g_raid_next = now + (ULONGLONG)(g_raid_minutes * 60000); raid_start(p); }
            } } catch (...) {} }
    }
}

// ------------------------------------------------------------------ presets + hotkeys (api.json "presets" / "hotkeys")
// presets: { id: { label, steps: [{cmd, args}], undo: [{cmd, args}] } }  - a preset is a list of trainer commands;
// running it again with "off" runs its undo list. hotkeys: { "F5": {preset | cmd+args | toggle: <toggle id>} } -
// polled on the game thread while the game window is in front (edge-triggered, Ctrl/Shift/Alt prefixes allowed).
static json tq_exec(const std::string& name, const json& a);
static std::set<std::string> g_presets_on;
static json run_preset(const std::string& id, int want /* -1 flip, 0 off, 1 on */) {
    const json pr = g_api.value("presets", json::object());
    if (!pr.contains(id)) return fail("NO_PRESET", id);
    const bool on = want < 0 ? !g_presets_on.count(id) : want == 1;
    const json steps = pr[id].value(on ? "steps" : "undo", json::array());
    json out = json::array(); int okn = 0;
    for (auto& st : steps) {
        std::string c = st.value("cmd", std::string()); if (c.rfind("BL.", 0) == 0) c = "TQ." + c.substr(3);
        json r = tq_exec(c, st.value("args", json::object()));
        out.push_back({ {"cmd", c}, {"ok", r.value("ok", false)}, {"msg", r.value("message", std::string())} });
        if (r.value("ok", false)) ++okn;
    }
    if (on) g_presets_on.insert(id); else g_presets_on.erase(id);
    const std::string label = pr[id].value("label", id);
    toast(label + (on ? " ON" : " OFF") + " (" + std::to_string(okn) + "/" + std::to_string(steps.size()) + ")");
    plog("preset " + id + (on ? " on" : " off"));
    return ok({ {"preset", id}, {"on", on}, {"steps", out} });
}
static int vk_of(const std::string& k0) {
    std::string k = k0; for (auto& c : k) c = (char)toupper((unsigned char)c);
    if (k.size() >= 2 && k[0] == 'F' && isdigit((unsigned char)k[1])) { const int n = std::stoi(k.substr(1)); if (n >= 1 && n <= 24) return VK_F1 + n - 1; }
    if (k.size() == 1 && (isalnum((unsigned char)k[0]))) return k[0];
    if (k.rfind("NUM", 0) == 0 && k.size() == 4 && isdigit((unsigned char)k[3])) return VK_NUMPAD0 + (k[3] - '0');
    static const std::map<std::string, int> m = { {"HOME", VK_HOME}, {"END", VK_END}, {"PAGEUP", VK_PRIOR}, {"PAGEDOWN", VK_NEXT}, {"DELETE", VK_DELETE},
        {"UP", VK_UP}, {"DOWN", VK_DOWN}, {"LEFT", VK_LEFT}, {"RIGHT", VK_RIGHT}, {"PAUSE", VK_PAUSE} };
    auto it = m.find(k); return it == m.end() ? 0 : it->second;
}
// Compiled once per api.json load: parsing the key names and four GetAsyncKeyState calls per hotkey every frame cost
// ~0.4 ms a frame (2026-11-18 BL.PERF, 15 hotkeys) - twice the whole cheat keeper. Now: each key read once, 50 Hz.
struct HotkeyDef { std::string id; int vk = 0; bool ctrl = false, shift = false, alt = false; bool was = false; };
static void hotkeys_tick() {
    static std::vector<HotkeyDef> defs; static unsigned gen = ~0u; static ULONGLONG last = 0;
    if (gen != g_api_gen) {
        gen = g_api_gen; std::vector<HotkeyDef> fresh;
        for (auto& kv : api_tbl("hotkeys").items()) {
            HotkeyDef d; d.id = kv.key(); std::string key = kv.key();
            for (;;) { const size_t p = key.find('+'); if (p == std::string::npos) break;
                std::string mod = key.substr(0, p); for (auto& c : mod) c = (char)tolower((unsigned char)c);
                if (mod == "ctrl") d.ctrl = true; else if (mod == "shift") d.shift = true; else if (mod == "alt") d.alt = true;
                key = key.substr(p + 1); }
            d.vk = vk_of(key); if (!d.vk) continue;
            for (auto& o : defs) if (o.id == d.id) d.was = o.was;
            fresh.push_back(std::move(d));
        }
        defs = std::move(fresh);
    }
    if (defs.empty()) return;
    const ULONGLONG now_ms = GetTickCount64(); if (now_ms - last < 20) return; last = now_ms;
    HWND fg = GetForegroundWindow(); DWORD pid = 0; GetWindowThreadProcessId(fg, &pid); if (pid != GetCurrentProcessId()) return;
    auto held = [](int vk) { return (GetAsyncKeyState(vk) & 0x8000) != 0; };
    const bool ctrl = held(VK_CONTROL), shift = held(VK_SHIFT), alt = held(VK_MENU);
    int seen_vk[32]; bool seen_dn[32]; int ns = 0;
    auto key_dn = [&](int vk) { for (int i = 0; i < ns; ++i) if (seen_vk[i] == vk) return seen_dn[i];
        const bool d = held(vk); if (ns < 32) { seen_vk[ns] = vk; seen_dn[ns] = d; ++ns; } return d; };
    for (auto& d : defs) {
        const bool now = key_dn(d.vk) && ctrl == d.ctrl && shift == d.shift && alt == d.alt;
        if (now && !d.was) {
            const json& hk = api_tbl("hotkeys"); auto it = hk.find(d.id);
            if (it != hk.end()) try {
                const json& h = *it;
                if (h.contains("preset")) run_preset(h["preset"].get<std::string>(), -1);
                else if (h.contains("toggle")) { const std::string id = h["toggle"].get<std::string>(); const bool on = !g_active.contains(id);
                    json r = set_toggle(id, on); toast(h.value("label", id) + (on ? " ON" : " OFF") + (r.value("ok", false) ? "" : " - " + r.value("message", std::string()))); }
                else if (h.contains("cmd")) { std::string c = h["cmd"].get<std::string>(); if (c.rfind("BL.", 0) == 0) c = "TQ." + c.substr(3);
                    json r = tq_exec(c, h.value("args", json::object())); toast(h.value("label", c) + (r.value("ok", false) ? " - done" : " - " + r.value("message", std::string("failed")))); }
            } catch (...) {}
        }
        d.was = now;
    }
}


// Data records live in NexusConfigStore_* objects. KismetSystemLibrary::IsValidGameDataHandle -> data manager
// (game+0xC83F388) slot 0x58 -> lookup game+0x59A1302: category -> store (registry game+0xC9A1AB0, +0x140 hash, +0xC8 stores),
// then store virtual +0x318 -> FindElement = game+0x4837648 (store, const FName* key, bool) : element* (hash at store+0x278,
// 32-byte entries {key idx, number, element*, next}).
static const uintptr_t kRvaFindElement = 0x4837648;
typedef void* (__fastcall* NexusFindFn)(void*, const void*, bool);
static void* nexus_find_raw(uintptr_t store, const ue::FName* key, bool flag) {
    __try { return ((NexusFindFn)(game_base() + kRvaFindElement))((void*)store, key, flag); }
    __except (1) { return (void*)1; }
}
static uintptr_t nexus_store(const std::string& nm) {
    for (const std::string cn : { "NexusConfigStore_" + nm, "NexusConfigStore" + nm }) {
        const uintptr_t cls = ue::find_class(cn); if (!cls) continue;
        for (uintptr_t o : ue::find_all(cls, false, 20)) if (ue::class_of(o) == cls) return o;
    }
    return 0;
}
static std::string hexdump(uintptr_t a, size_t n) {
    std::vector<uint8_t> b(n); if (!ue::mread(a, b.data(), n)) return "";
    return to_hex(b.data(), n);
}

// Records sit in TSets inside the store object: {data*, num, max} with 32-byte entries {u64 name hash, 0, record*, link}.
// A record is a typed C++ struct; FNames inside it (8 bytes {index, number}) name its parts and the defs it points to.
// nexus_scan walks every set, reads each record + the small arrays it points to and returns records whose own names
// match a prefix, with every name found inside (depth 0 = the record, depth 1 = arrays it points to).
static bool plausible_name(const ue::FName& n, std::string& out) {
    if (n.index == 0 || n.index > 0x8000000 || n.number > 64) return false;
    static thread_local std::unordered_map<uint32_t, std::string> cache;   // per thread: BL.NEXUS_SCAN runs off the game thread
    auto it = cache.find(n.index); std::string s;
    if (it != cache.end()) s = it->second; else { ue::FName z{ n.index, 0 }; s = ue::name_str(z); if (cache.size() < 400000) cache[n.index] = s; }
    if (s.size() < 3 || s.size() > 160 || s == "None") return false;
    for (char c : s) if ((unsigned char)c < 0x20 || (unsigned char)c > 0x7E) return false;
    out = n.number ? s + "_" + std::to_string(n.number - 1) : s; return true;
}
static void names_in(const uint8_t* b, size_t n, std::vector<std::string>& out, size_t cap) {
    for (size_t o = 0; o + 8 <= n && out.size() < cap; o += 4) { ue::FName f; memcpy(&f, b + o, 8); std::string s; if (plausible_name(f, s)) out.push_back(s); }
}
static json nexus_scan(const json& a) {
    const std::string st = a.value("store", std::string("GbxActor")), pre = a.value("prefix", std::string("Mix_"));
    const uintptr_t store = nexus_store(st); if (!store) return fail("NO_STORE", "no live store " + st);
    const size_t rec_len = a.value("rec_len", 0x400), maxout = a.value("max", 400);
    json out = json::array(); json sets = json::array(); int records = 0;
    for (int off = 0x40; off < 0x320; off += 8) {   // find TSets: {ptr, num, max} with 32-byte entries whose +0x10 is a pointer
        uintptr_t data = 0; int32_t num = 0, mx = 0; ue::rd_as(store + off, data); ue::rd_as(store + off + 8, num); ue::rd_as(store + off + 12, mx);
        if (!data || num < 1 || num > 200000 || mx < num) continue;
        uintptr_t r0 = 0; if (!ue::rd_as(data + 0x10, r0) || r0 < 0x10000) continue;
        sets.push_back({ {"off", hexstr(off)}, {"num", num} });
        std::vector<uint8_t> buf(rec_len), sub(4096);
        for (int i = 0; i < mx && out.size() < maxout; ++i) {
            uintptr_t rec = 0; uint64_t h = 0; if (!ue::rd_as(data + i * 32 + 0x10, rec) || !rec) continue; ue::rd_as(data + i * 32, h);
            if (!ue::mread(rec, buf.data(), rec_len)) continue; ++records;
            std::vector<std::string> own; names_in(buf.data(), rec_len, own, 64);
            bool hit = pre.empty(); for (auto& n : own) if (n.rfind(pre, 0) == 0) { hit = true; break; }
            if (!hit) continue;
            std::vector<std::string> deep;
            for (size_t o = 0; o + 16 <= rec_len; o += 8) {   // TArray-looking {ptr, num, max}
                uintptr_t p = 0; int32_t n2 = 0, m2 = 0; memcpy(&p, buf.data() + o, 8); memcpy(&n2, buf.data() + o + 8, 4); memcpy(&m2, buf.data() + o + 12, 4);
                if (p < 0x10000000000ull || p > 0x7FFFFFFFFFFFull || n2 < 1 || n2 > 256 || m2 < n2) continue;
                const size_t len = std::min<size_t>(4096, (size_t)n2 * 96);
                if (!ue::mread(p, sub.data(), len)) continue;
                std::vector<std::string> d; names_in(sub.data(), len, d, 256);
                for (auto& x : d) deep.push_back(x);
            }
            out.push_back({ {"set", hexstr(off)}, {"hash", hexstr(h)}, {"record", hexstr(rec)}, {"names", own}, {"inner", deep} });
        }
    }
    return ok({ {"store", hexstr(store)}, {"sets", sets}, {"records_read", records}, {"matches", out.size()}, {"records", out} });
}


// name -> record, with the game's own key code (from the IsValidGameDataHandle path, game+0x59A13E6 / 0x59A1498):
//   game+0x46DCDD4 (key* out, const FName* in) builds a key object; game+0x4A659AC (key*, out {u64 hash, u32 extra}, int part)
//   gives the hash the store sets are keyed by (set entry = {u64 hash, 0, record*, link}).
static const uintptr_t kRvaMakeKey = 0x46DCDD4, kRvaKeyHash = 0x4A659AC;
typedef void (__fastcall* NexusMakeKeyFn)(void*, const void*);
typedef void* (__fastcall* NexusKeyHashFn)(void*, void*, int);
static bool nexus_hash_raw(const ue::FName* fn, uint8_t* keybuf, uint8_t* out) {
    __try {
        ((NexusMakeKeyFn)(game_base() + kRvaMakeKey))(keybuf, fn);
        ((NexusKeyHashFn)(game_base() + kRvaKeyHash))(keybuf, out, 0);
        return true;
    } __except (1) { return false; }
}
static bool nexus_hash(const std::string& name, uint64_t& h, uint32_t& extra) {
    ue::FName fn{}; if (!ue::name_find(name, fn)) return false;
    static uint8_t key[0x200]; uint8_t out[16] = {};
    memset(key, 0, sizeof key);
    if (!nexus_hash_raw(&fn, key, out)) return false;
    memcpy(&h, out, 8); memcpy(&extra, out + 8, 4); return true;
}
static uintptr_t nexus_record(uintptr_t store, uint64_t h, int* set_off = nullptr) {
    for (int off = 0x40; off < 0x320; off += 8) {
        uintptr_t data = 0; int32_t num = 0, mx = 0; ue::rd_as(store + off, data); ue::rd_as(store + off + 8, num); ue::rd_as(store + off + 12, mx);
        if (!data || num < 1 || num > 200000 || mx < num) continue;
        for (int i = 0; i < mx; ++i) { uint64_t k = 0; if (!ue::rd_as(data + i * 32, k) || k != h) continue;
            uintptr_t rec = 0; ue::rd_as(data + i * 32 + 0x10, rec); if (rec) { if (set_off) *set_off = off; return rec; } }
    }
    return 0;
}
static json nexus_get(const json& a) {
    const std::string st = a.value("store", std::string("GbxActor")), nm = a.value("name", std::string());
    const uintptr_t store = nexus_store(st); if (!store) return fail("NO_STORE", "no live store " + st);
    // !! calling the game's key builder (game+0x46DCDD4) from here HUNG the game thread on 2026-11-10 -> only with unsafe:true
    if (!a.value("unsafe", false)) return fail("UNSAFE", "the game's key builder hung the game when called from the trainer; pass unsafe:true only to research it");
    uint64_t h = 0; uint32_t extra = 0; if (!nexus_hash(nm, h, extra)) return fail("HASH", "could not hash " + nm);
    int so = 0; const uintptr_t rec = nexus_record(store, h, &so);
    json o = { {"hash", hexstr(h)}, {"extra", extra}, {"record", hexstr(rec)}, {"set", hexstr(so)} };
    if (rec) {
        const size_t len = a.value("len", 0x100); o["bytes"] = hexdump(rec, len);
        std::vector<uint8_t> b(len); ue::mread(rec, b.data(), len); std::vector<std::string> nn; names_in(b.data(), len, nn, 64); o["names"] = nn;
    }
    return ok(o);
}


// ------------------------------------------------------------------ SPAWN LISTS (the game's Mix_ records, decoded 2026-11-10)
// A GameDataHandle is {FName, u32 category, record* cache}: when the game resolves it (e.g. SpawnerComponent::SetGbxActorDef)
// the third qword caches the record pointer. One loaded spawner is borrowed as a "resolver" (set -> read cache -> restore).
// Mix record: TArray<entry> at +0x90; entry = 0xF8 bytes:
//   +0x00 FName def (Char_ or Mix_), +0x08 u32 category (0x4000), +0x10 record* (resolved; sub-lists followed by pointer)
//   +0x18 f32 weight, +0x48 FName condition attribute (Attr_Infamy_MixVariety_<Faction>_<None|Low|Med>, Attr_Badass_Limit...)
//   +0x60 f32 weight when the condition applies
static json mix_entries(uintptr_t rec, int depth) {
    json out = json::array(); if (!rec || depth > 4) return out;
    uintptr_t arr = 0; int32_t n = 0; ue::rd_as(rec + 0x90, arr); ue::rd_as(rec + 0x98, n);
    if (!arr || n < 0 || n > 256) return out;
    for (int i = 0; i < n; ++i) {
        const uintptr_t e = arr + (uintptr_t)i * 0xF8;
        ue::FName def{}, cond{}; uint32_t cat = 0; uintptr_t sub = 0; float w = 0, w2 = 0;
        ue::rd_as(e, def); ue::rd_as(e + 8, cat); ue::rd_as(e + 0x10, sub); ue::rd_as(e + 0x18, w); ue::rd_as(e + 0x48, cond); ue::rd_as(e + 0x60, w2);
        std::string dn; if (!plausible_name(def, dn)) continue;
        json j = { {"def", dn}, {"weight", std::round(w * 1000) / 1000} };
        std::string cn; if (plausible_name(cond, cn)) { j["if"] = cn; j["if_weight"] = std::round(w2 * 1000) / 1000; }
        if (dn.rfind("Mix_", 0) == 0) j["list"] = mix_entries(sub, depth + 1);
        out.push_back(j);
    }
    return out;
}
static uintptr_t g_resolver = 0; static ue::FName g_resolver_orig{};
static uintptr_t resolve_actor_def(const std::string& name) {
    if (!g_resolver || !ue::alive(g_resolver)) {
        g_resolver = 0;
        for (uintptr_t sc : ue::find_all(ue::find_class("OakSpawnerComponent"), false, 4000)) { if (spawner_def(sc).rfind("Mix_", 0) == 0) { g_resolver = sc; break; } }
        if (!g_resolver) return 0;
        spawner_def_off(g_resolver); ue::rd_as(g_resolver + g_spawner_def_off, g_resolver_orig);
    }
    if (!ue::call_json(g_resolver, "SetGbxActorDef", { {"NewGbxActorDef", "gdh:" + name + ":16384"}, {"bDestroyExistingActors", false} }, 0).value("ok", false)) return 0;
    uintptr_t rec = 0; ue::rd_as(g_resolver + g_spawner_def_off + 0x10, rec);
    return rec;
}
static void resolver_restore() {
    if (g_resolver && ue::alive(g_resolver)) ue::call_json(g_resolver, "SetGbxActorDef", { {"NewGbxActorDef", "gdh:" + ue::name_str(g_resolver_orig) + ":16384"}, {"bDestroyExistingActors", false} }, 0);
}
static json spawnlists_cmd(const json& a) {
    json s0 = session(); if (!s0.value("in_game", false)) return fail("NOT_IN_GAME", "load a character first");
    // It borrows a loaded spawner and calls SetGbxActorDef on it - the call that corrupted the heap when a level streamed out
    // during a teleport (see enemy_tick). So: a write (solo only), only on request, and the spawner is not kept afterwards.
    if (!s0.value("standalone", false)) return fail("NOT_SOLO", "solo only (it changes a live spawner for a moment)");
    if (!a.value("unsafe", false)) return fail("UNSAFE", "BL.SPAWNLISTS borrows a loaded enemy spawner and calls SetGbxActorDef on it; that call crashed the game "
        "when the level unloaded during a teleport. panel\\spawnlists.json already holds the dumped lists. To dump again: stand still in a loaded area "
        "(no travel / teleport) and pass unsafe:true");
    json names = a.value("names", json::array()); if (!names.is_array()) names = json::array(); if (a.contains("name")) names.push_back(a["name"]);
    json out = json::object(); int bad = 0;
    for (auto& nj : names) {
        if (!nj.is_string()) { ++bad; continue; }
        const std::string nm = nj.get<std::string>(); ue::FName fn{}; if (!ue::name_find(nm, fn)) { ++bad; continue; }
        const uintptr_t rec = resolve_actor_def(nm); if (!rec) { ++bad; continue; }
        out[nm] = mix_entries(rec, 0);
    }
    resolver_restore();
    g_resolver = 0;   // never reused later: the spawner can be streamed out by then
    return ok({ {"lists", out}, {"unresolved", bad} });
}

static json nexus_cmd(const json& a) {
    const std::string st = a.value("store", std::string("GbxActor")), nm = a.value("name", std::string());
    const uintptr_t store = nexus_store(st); if (!store) return fail("NO_STORE", "no live NexusConfigStore_" + st);
    ue::FName fn{}; if (!ue::name_find(nm, fn)) return fail("NO_NAME", "name not in the pool: " + nm);
    void* el = nexus_find_raw(store, &fn, a.value("flag", false));
    json o = { {"store", hexstr(store)}, {"element", hexstr((uintptr_t)el)} };
    if ((uintptr_t)el == 1) return fail("FAULT", "FindElement faulted");
    if (el) {
        o["element_bytes"] = hexdump((uintptr_t)el, 0x80);
        uintptr_t p58 = 0; ue::rd_as((uintptr_t)el + 0x58, p58); o["p58"] = hexstr(p58);
        if (p58) { o["p58_bytes"] = hexdump(p58, 0x40); uintptr_t p8 = 0; ue::rd_as(p58 + 8, p8); o["p58_8"] = hexstr(p8); }
    }
    return ok(o);
}

static json tq_exec(const std::string& name, const json& a) {
        static const std::set<std::string> kDevOnly = { "TQ.COMPNAMES", "TQ.PARTPROBE", "TQ.DEFPROBE", "TQ.DEF_STORES", "TQ.DEF_LIST",
            "TQ.DEF_GET", "TQ.DEFIDX", "TQ.NAMES", "TQ.TABLE_GET", "TQ.NEXUS", "TQ.NEXUS_GET", "TQ.NEXUS_SCAN", "TQ.PICKUP_INFO",
            "TQ.PERF", "TQ.API", "TQ.API_RELOAD", "TQ.FRAMES" };
        if (kDevOnly.count(name)) return fail("NOT_IN_BUILD", name + " is a developer command");
        if (name.rfind("TQ.SAVE", 0) == 0) return wb::sv::cmd(name, a);
        if (name.rfind("TQ.ITEM_", 0) == 0) return wb::items::cmd(name, a);
        if (name == "TQ.STATE" || name == "DK.STATUS") return ok(state_now());
        if (name == "TQ.SESSION" || name == "DK.SESSION") return ok(session());
        if (name == "TQ.CALL") return run_action(a.value("name", ""), a.value("args", json::object()), a.value("v", json()));
        if (name == "TQ.TOGGLE") return set_toggle(a.value("id", ""), a.contains("v") ? a["v"] : json(true));
        if (name == "TQ.TOGGLES") {
            json out = json::object(); const json& tg = api_tbl("toggles");
            for (auto& kv : tg.items()) out[kv.key()] = { {"desc", kv.value().value("desc", "")}, {"active", g_active.contains(kv.key()) ? g_active[kv.key()] : json(false)} };
            return ok(out);
        }
        if (name == "TQ.TELEPORT") return teleport_to(a.value("x", 0.0), a.value("y", 0.0), a.value("z", 0.0));
        if (name == "TQ.POS") return player_pos();
        if (name == "TQ.POS_SAVE" || name == "TQ.POS_LOAD") {   // saved positions (<data>\positions.json), {slot} default "1"
            const std::string f = g_data_dir + "\\positions.json"; json all = read_json(f); if (!all.is_object()) all = json::object();
            const std::string slot = a.contains("slot") ? (a["slot"].is_string() ? a["slot"].get<std::string>() : a["slot"].dump()) : "1";
            if (name == "TQ.POS_SAVE") {
                json p = player_pos(); if (!p.value("ok", false)) return p;
                all[slot] = p["data"].contains("value") ? p["data"]["value"] : p["data"];
                save_toggles(); std::ofstream(f) << all.dump(1); return ok({ {"slot", slot}, {"saved", all[slot]} });
            }
            if (!all.contains(slot)) return fail("NO_SLOT", "no saved position '" + slot + "'");
            const json& v = all[slot]; return teleport_to(v.value("X", 0.0), v.value("Y", 0.0), v.value("Z", 0.0) + 30);
        }
        if (name == "TQ.LOOT") return loot_spawn(a);
        if (name == "TQ.LOOT_RESULT") return ok(g_last_loot);
        if (name == "TQ.CATALOG") return catalog_cmd(a);
        if (name == "TQ.PICKUPS") return pickups_cmd(a);
        if (name == "TQ.VACUUM") return vacuum(a);
        if (name == "TQ.ATTR") {   // {name, v, mode: set|mult|add|scale_add} | {name, reset:true} | {name} = read
            if (!a.contains("v") && !a.contains("reset")) { json v = attr_get(a.value("name", std::string())); return v.is_object() ? fail("NO_ATTR", v.value("error", std::string())) : ok({ {"value", v} }); }
            return attr_set(a);
        }
        if (name == "TQ.ATTR_CLEAR") return attr_clear(a);
        if (name == "TQ.SKILL_CD_TICK") {   // toggle fast_cooldown: the cooldown pool (seconds, 28 = full) gets (v - 1) x elapsed seconds
            static ULONGLONG last = 0; const ULONGLONG now = GetTickCount64();
            const double dt = (last && now > last) ? std::min(1.0, (now - last) / 1000.0) : 0.25; last = now;
            const double sp = a.contains("v") && a["v"].is_number() ? std::clamp(a["v"].get<double>(), 1.0, 50.0) : 1.0;
            if (sp <= 1.0) return ok(nullptr);
            const uintptr_t pawn = player_pawn(); if (!pawn) return fail("NO_PAWN", "no player");
            static uintptr_t lib = 0; if (!lib || !ue::alive(lib)) lib = ue::cdo_of(ue::find_class("GbxSkillComponentFunctions_ActionSkill"));
            if (!lib) return fail("NO_LIB", "GbxSkillComponentFunctions_ActionSkill");
            return ue::call_json(lib, "RefillCooldownByValue", { {"target", "addr:" + hexstr(pawn)}, {"Value", (sp - 1.0) * dt} }, 0); }
        if (name == "TQ.ATTR_CLEAR_SOME") {   // {names: [...]}: those modifiers off, the rest kept
            json out = json::object(); if (!a.contains("names") || !a["names"].is_array()) return fail("BAD_ARG", "names = list");
            for (auto& n : a["names"]) if (n.is_string()) out[n.get<std::string>()] = attr_set({ {"name", n}, {"reset", true} }).value("ok", false);
            return ok(out); }
        if (name == "TQ.ATTRS") {   // active modifiers + their live values
            json out = json::object(); for (auto& [k, m] : g_mods) out[k] = { {"mode", m.type}, {"value", m.value}, {"live", attr_get(k)} };
            return ok(out);
        }
        if (name == "TQ.ENEMIES") return enemies_cmd(a, false);
        if (name == "TQ.KILL") return enemies_cmd(a, true);
        if (name == "TQ.FREEZE") return freeze_cmd(a);
        if (name == "TQ.FLY") return fly_cmd(a);
        if (name == "TQ.STATIONS") return stations_cmd(a);
        if (name == "TQ.SPAWN") return spawn_cmd(a);
        if (name == "TQ.DIFF") return diff_cmd(a);
        if (name == "TQ.PERF") return perf_cmd(a);
        if (name == "TQ.FRAMES") return frames_cmd(a);
        if (name == "TQ.CAMERA") return camera_cmd(a);
        if (name == "TQ.PLACES") {
            const size_t k = g_api_path.find_last_of("\\/"); json j = read_json(g_api_path.substr(0, k) + "\\places.json");
            return j.is_object() ? ok(j) : fail("NO_FILE", "places.json is missing from the trainer folder"); }
        if (name == "TQ.NEXUS") return nexus_cmd(a);
        if (name == "TQ.SPAWNLISTS") return spawnlists_cmd(a);
        if (name == "TQ.NEXUS_GET") return nexus_get(a);
        if (name == "TQ.ENEMY_LIST") return enemy_list_cmd(a);
        if (name == "TQ.ENEMY") return enemy_cmd(a);
        if (name == "TQ.ENEMY_SPAWN") return enemy_spawn_cmd(a);
        if (name == "TQ.SPAWNERS") return spawners_cmd(a);
        if (name == "TQ.SPAWN_SWAP") return swap_cmd(a);
        if (name == "TQ.ENEMY_CATALOG") { const std::string f = lower(a.value("filter", std::string())), k = a.value("kind", std::string()), t = a.value("team", std::string()); json out = json::array();
            for (auto& e : enemy_cat()) { if (!k.empty() && e.value("kind", "") != k) continue; if (!t.empty() && e.value("team", "") != t) continue;
                if (!f.empty() && lower(e.value("name", "")).find(f) == std::string::npos) continue; out.push_back(e); if (out.size() >= (size_t)a.value("max", 400)) break; }
            return ok({ {"count", out.size()}, {"enemies", out} }); }
        if (name == "TQ.ACTORS") { const std::string f = lower(a.value("filter", std::string())); json out = json::array();
            for (auto& x : actors()) { const std::string n = x.is_string() ? x.get<std::string>() : x.value("name", ""); if (f.empty() || lower(n).find(f) != std::string::npos) out.push_back(n); if (out.size() >= 300) break; }
            return ok({ {"count", out.size()}, {"actors", out} }); }
        if (name == "TQ.TIME") return time_cmd(a);
        if (name == "TQ.WAYPOINT") return waypoint_cmd(a);
        if (name == "TQ.MISSIONS") return missions_cmd(a);
        if (name == "TQ.SERIALS") return serials_cmd(a);
        if (name == "TQ.VAULTCARD") return vaultcard_cmd(a);
        if (name == "TQ.POINTS") return points_cmd(a);
        if (name == "TQ.NAMES") return display_names_cmd(a);   // in-game item names -> Gearbox names
        if (name.rfind("TQ.DEF_", 0) == 0) return def_cmd(name, a);   // game design data by name
        if (name.rfind("TQ.TABLE_", 0) == 0) return def_cmd(name, a);   // balance tables (perk chances, unique damage ...)
        if (name == "TQ.PART_INFO") return def_cmd(name, a);
        if (name == "TQ.VALUES" || name == "TQ.VALUE_SET" || name == "TQ.VALUE_RESET") { if (g_def_idx.empty()) def_index_build(); return ovr_cmd(name, a); }   // value edits that stay
        if (name == "TQ.DEFIDX") return defidx_cmd(a);
        if (name == "TQ.ITEMDB") return itemdb_cmd(a);
        if (name == "TQ.HUD") return hud_cmd(a);
        if (name == "TQ.BUILD") return build_cmd(a);
        if (name == "TQ.BUILD_RESULT") return ok({ {"busy", g_build.active}, {"tries", g_build.tries}, {"result", g_build.result} });
        if (name == "TQ.BUILDER") return builder_info(a);
        if (name == "TQ.BUILDER_SET") {   // panel drop-downs: {key: maker|type|rarity|level|legendary, v}
            const std::string k = a.value("key", std::string()); if (!g_builder.contains(k)) return fail("BAD_KEY", k);
            json v = a.contains("v") ? a["v"] : json();   // the right type only: build_cmd reads numbers / text and threw on anything else
            if (k == "rarity" || k == "level") {
                if (v.is_string()) { const std::string s = v.get<std::string>(); char* e = nullptr; const double d = strtod(s.c_str(), &e); if (e && e != s.c_str()) v = d; }
                if (!v.is_number()) return fail("BAD_ARG", k + " needs a number");
                v = (int)std::clamp(v.get<double>(), 0.0, 1000.0);
            } else if (!v.is_string()) v = v.is_number() ? json(std::to_string((long long)v.get<double>())) : json("");
            g_builder[k] = v; if (k != "legendary" && a.value("clear_legendary", true)) g_builder["legendary"] = "";
            return ok(g_builder); }
        if (name == "TQ.PICKUP_INFO") { json out = json::array(); for (uintptr_t x : all_pickups()) { json i = pickup_info(x); i["addr"] = hexstr(x); out.push_back(i); } return ok(out); }
        if (name == "TQ.CONSOLE") {   // engine KismetSystemLibrary::ExecuteConsoleCommand (cvars / Gearbox console commands)
            json s = session(); if (!s.value("standalone", false)) return fail("NOT_SOLO", "solo only");
            const uintptr_t ksl = ue::cdo_of(ue::find_class("KismetSystemLibrary")); uintptr_t pc = player_pc();
            json r = ue::call_json(ksl, "ExecuteConsoleCommand", { {"WorldContextObject", "addr:" + hexstr(player_pawn())}, {"Command", a.value("cmd", std::string())}, {"SpecificPlayer", "addr:" + hexstr(pc)} }, 0);
            plog("console: " + a.value("cmd", std::string())); return r; }
        if (name == "TQ.PRESET") return run_preset(a.value("name", std::string()), a.contains("on") ? (a["on"].get<bool>() ? 1 : 0) : -1);
        if (name == "TQ.PRESETS") { json out = json::object(); const json pr = g_api.value("presets", json::object());
            for (auto& kv : pr.items()) out[kv.key()] = { {"label", kv.value().value("label", kv.key())}, {"on", g_presets_on.count(kv.key()) > 0} };
            return ok({ {"presets", out}, {"hotkeys", g_api.value("hotkeys", json::object())} }); }
        if (name == "TQ.TRAVEL") return travel_cmd(a);
        if (name == "TQ.ENUM") return enum_values(a.value("name", ""));
        if (name == "TQ.TARGET") {
            std::string why; uintptr_t o = target(a.value("target", "player.pawn"), &why);
            return o ? ok({ {"addr", hexstr(o)}, {"name", ue::full_name(o)} }) : fail("NO_TARGET", why);
        }
        return fail("UNKNOWN", name + ": unknown command");
}

void post(const json& requests) {
    if (!requests.is_array()) return;
    for (auto& r : requests) {
        const uint64_t seq = r.value("seq", (uint64_t)0);
        const std::string op = r.value("op", ""), id = r.value("id", "");
        json req = r;
        // panel inputs send text: a pure number becomes a number ("50" -> 50, "1.5" -> 1.5); names stay text
        if (req.contains("v") && req["v"].is_string()) {
            const std::string s = req["v"].get<std::string>(); char* end = nullptr;
            const double d = s.empty() ? 0 : strtod(s.c_str(), &end);
            if (!s.empty() && end && *end == 0) req["v"] = d;
        }
        run_on_main([seq, op, id, req]() -> json {
            try {
            const json* it = spec_item(id);
            if ((op == "click" || op == "set") && it && it->contains("tq")) {
                json args = it->value("args", json::object());
                if (op == "set" && it->contains("arg")) args[(*it)["arg"].get<std::string>()] = req.value("v", json());
                json res = tq_exec((*it)["tq"].get<std::string>(), args);
                put_result(seq, res.value("ok", false), res.value("ok", false) ? "done" : res.value("message", std::string("failed")));
            } else if (op == "set" && it && it->contains("toggle")) {
                json v = req.value("v", json());
                if (v.is_boolean() || v.is_number()) {
                    json res = (v.is_boolean() && !v.get<bool>()) ? set_toggle((*it)["toggle"].get<std::string>(), false) : set_toggle((*it)["toggle"].get<std::string>(), v);
                    put_result(seq, res.value("ok", false), res.value("ok", false) ? "done" : res.value("message", std::string("failed")));
                } else put_result(seq, false, "needs on / off or a number");
            } else if ((op == "click" || op == "set") && it && it->contains("api")) {
                json v = op == "set" ? req.value("v", json()) : json();
                json res = run_action((*it)["api"].get<std::string>(), it->value("args", json::object()), v);
                put_result(seq, res.value("ok", false), res.value("ok", false) ? "done" : res.value("message", std::string("failed")));
            } else if (op == "all_off") {
                std::vector<std::string> ids; for (auto& kv : g_active.items()) ids.push_back(kv.key());
                for (auto& id : ids) set_toggle(id, false);
                put_result(seq, true, "switched off " + std::to_string(ids.size()));
            } else {
                put_result(seq, false, "not wired yet (no \"api\" on this item)");
            }
            } catch (std::exception& e) { put_result(seq, false, std::string("failed: ") + e.what()); }   // the panel request always gets an answer
            catch (...) { put_result(seq, false, "failed"); }
            return json();
        }, 8000);
    }
}

json state() {
    std::lock_guard<std::mutex> lk(g_mtx);
    json vals = json::object();
    { const json tabs = g_spec_json.value("tabs", json::array());
      for (auto& tb : tabs) { const json secs = tb.value("sections", json::array());
        for (auto& s : secs) { const json items = s.value("items", json::array());
          for (auto& it : items) {
              if (it.contains("toggle")) { const std::string tid = it["toggle"].get<std::string>();
                  vals[it.value("id", "")] = g_active.contains(tid) ? g_active[tid] : json(false); }
              else if (it.contains("cam")) { const std::string ck = it["cam"].get<std::string>(); if (g_cam_snap.contains(ck)) vals[it.value("id", "")] = g_cam_snap[ck]; }
              else if (it.contains("diff")) { const std::string dk = it["diff"].get<std::string>();   // difficulty sliders follow the preset
                  if (g_diff_snap.contains(dk)) vals[it.value("id", "")] = g_diff_snap[dk]; } } } } }
    return { {"seq_done", g_seq_done}, {"results", g_results}, {"values", vals}, {"readouts", g_ro_cache},
             {"keeper", { {"ready", ue::ready()}, {"note", "BL4 trainer"} }}, {"player", json::object()}, {"panel_log", g_log} };
}

uint64_t state_version() { return g_ver.load(); }

json cmd(const std::string& name0, const json& a) {
    const std::string name = name0.rfind("BL.", 0) == 0 ? "TQ." + name0.substr(3) : name0;
    if (name.rfind("TQ.SAVE", 0) == 0) return wb::sv::cmd(name, a);
    if (name.rfind("TQ.ITEM_", 0) == 0) return wb::items::cmd(name, a);   // serial editor (no engine needed)   // save editor: works at the title screen too
    if (!ue::init()) return fail("NOT_READY", "engine core not ready");
    if (name == "TQ.NEXUS_SCAN" || name == "TQ.API" || name == "TQ.API_RELOAD") return fail("NOT_IN_BUILD", name + " is a developer command");
    if (name == "TQ.NEXUS_SCAN") {   // memory reads only: runs on the caller's thread (takes longer than the main-thread budget)
        json r = nexus_scan(a);
        if (a.contains("file") && r.value("ok", false)) { std::ofstream(a["file"].get<std::string>()) << r["data"].dump(); json o = r["data"]; o.erase("records"); o["file"] = a["file"]; return ok(o); }
        return r; }
    // both touch g_api / g_spec_json, which the game thread reads every tick: on the game thread like every other command
    if (name == "TQ.API_RELOAD") return run_on_main([]() -> json { load_api(true); load_spec(); return ok(g_api); }, 8000);
    if (name == "TQ.API") return run_on_main([]() -> json {
        json acts = json::object(), ros = json::object();
        const json A = g_api.value("actions", json::object()), R = g_api.value("readouts", json::object());
        for (auto& kv : A.items())
            acts[kv.key()] = { {"desc", kv.value().value("desc", "")}, {"arg", kv.value().value("arg", "")}, {"write", kv.value().value("write", true)} };
        for (auto& kv : R.items()) ros[kv.key()] = kv.value().value("desc", "");
        return ok({ {"actions", acts}, {"readouts", ros}, {"file", g_api_path} });
    }, 8000);
    return run_on_main([name, a]() -> json { return tq_exec(name, a); }, 8000);
}

} // namespace dk

// ------------------------------------------------------------------ HUD drawing (render thread)
#include "imgui.h"
namespace wb {
bool hud_live() { if (dk::g_rain.on.load() || dk::g_rain.tint.load() || dk::g_tp_xhair.load()) return true; std::lock_guard<std::mutex> lk(dk::g_hud_mtx); return !dk::g_hud_marks.empty() && GetTickCount64() - dk::g_hud_at < 1000; }
// MATRIX code rain (render thread). Port of the plugin's HLSL: cells of size x 1.33 size, column seed
// frac(sin(x*12.9898)*43758.5453), speed 5+seed*13 rows/s, head = fmod(t*speed + seed*977, rows+40) - 20,
// trail 8+seed*18 rows fading with pow(1-d/trail, 1.6), glyph changes at floor(t*(2+seed*6) + row*0.37), pale head.
static float rain_frac(float v) { return v - std::floor(v); }
static float rain_hash1(float v) { return rain_frac(std::sin(v * 12.9898f) * 43758.5453f); }
static float rain_hash3(float a, float b, float c) { return rain_frac(std::sin(a * 127.1f + b * 311.7f + c * 74.7f) * 43758.5453f); }
static void rain_draw() {
    if (!dk::g_rain.on.load() || !ImGui::GetCurrentContext()) return;
    static const char kGlyphs[] = "0123456789ABCDEFHKMNPRSTVXZ$#@%&*+=<>:;|/\\{}[]?!";
    const int nG = (int)sizeof(kGlyphs) - 1;
    const ImVec2 D = ImGui::GetIO().DisplaySize; if (D.x < 64 || D.y < 64) return;
    const float sz = std::clamp(dk::g_rain.size.load(), 8.f, 48.f) * std::max(1.f, D.y / 1080.f), cw = sz * 0.8f, ch = sz * 1.05f;
    const float dens = std::clamp(dk::g_rain.density.load(), 0.05f, 3.f), spd = std::clamp(dk::g_rain.speed.load(), 0.1f, 5.f);
    const float alpha = std::clamp(dk::g_rain.alpha.load(), 0.05f, 1.f);
    const int cols = (int)(D.x / cw) + 1, rows = (int)(D.y / ch) + 1;
    const float t = (float)((GetTickCount64() % 3600000ULL) / 1000.0);
    ImDrawList* dl = ImGui::GetBackgroundDrawList(); ImFontAtlas* fa = ImGui::GetIO().Fonts;
    ImFont* font = fa->Fonts.Size > 1 ? fa->Fonts[1] : ImGui::GetFont();   // semibold reads better as code
    dl->AddRectFilled(ImVec2(0, 0), D, IM_COL32(0, 14, 3, (int)(110 * alpha)));          // dark green wash
    for (float y = 0; y < D.y; y += 3.f) dl->AddLine(ImVec2(0, y), ImVec2(D.x, y), IM_COL32(0, 0, 0, (int)(40 * alpha)));   // scanlines
    const int layers = (int)std::ceil(dens);
    char g[2] = { 0, 0 };
    for (int L = 0; L < layers; ++L) for (int c = 0; c < cols; ++c) {
        const float seed = rain_hash1((float)c + L * 57.31f);
        if (rain_hash1(seed * 91.7f + L) > dens - L) continue;                                 // density: share of active columns
        const float speed = (5.f + seed * 13.f) * spd;
        const float head = std::fmod(t * speed + seed * 977.f, (float)rows + 40.f) - 20.f;
        const float trail = 8.f + seed * 18.f;
        const int r0 = std::max(0, (int)std::floor(head - trail)), r1 = std::min(rows - 1, (int)std::floor(head));
        for (int r = r0; r <= r1; ++r) {
            const float d = head - (float)r; if (d < 0 || d > trail) continue;
            const float fade = std::pow(1.f - d / trail, 1.6f);
            const float step = std::floor(t * (2.f + seed * 6.f) + r * 0.37f);
            g[0] = kGlyphs[(int)(rain_hash3((float)c, (float)r, step) * nG) % nG];
            const bool isHead = d < 1.f;
            const float a = std::min(1.f, 0.25f + fade) * alpha;
            const ImU32 col = isHead ? IM_COL32(215, 255, 225, (int)(255 * alpha))
                                     : IM_COL32((int)(20 + 60 * fade), (int)(150 + 105 * fade), (int)(50 + 50 * fade), (int)(255 * a));
            const ImVec2 at((float)c * cw, (float)r * ch);
            if (isHead || fade > 0.75f) { dl->AddText(font, sz, ImVec2(at.x + 1.f, at.y), IM_COL32(90, 255, 120, (int)(110 * alpha)), g); dl->AddText(font, sz, ImVec2(at.x - 1.f, at.y + 1.f), IM_COL32(90, 255, 120, (int)(80 * alpha)), g); }   // glow
            dl->AddText(font, sz, at, col, g);
        }
    }
}
static void tp_crosshair_draw() {   // third-person aim reticle at screen centre (where the camera ray - and the shots - go)
    if (!dk::g_tp_xhair.load() || !ImGui::GetCurrentContext()) return;
    const ImVec2 D = ImGui::GetIO().DisplaySize; const ImVec2 c(D.x * 0.5f, D.y * 0.5f); const float k = std::max(1.f, D.y / 1080.f);
    ImDrawList* dl = ImGui::GetForegroundDrawList();
    const ImU32 sh = IM_COL32(0, 0, 0, 160), fg = IM_COL32(235, 250, 255, 235);
    const float g = 7.f * k, L = 13.f * k;
    for (int pass = 0; pass < 2; ++pass) {
        const ImU32 col = pass ? fg : sh; const float w = (pass ? 2.2f : 4.6f) * k;
        dl->AddLine(ImVec2(c.x - g - L, c.y), ImVec2(c.x - g, c.y), col, w); dl->AddLine(ImVec2(c.x + g, c.y), ImVec2(c.x + g + L, c.y), col, w);
        dl->AddLine(ImVec2(c.x, c.y - g - L), ImVec2(c.x, c.y - g), col, w); dl->AddLine(ImVec2(c.x, c.y + g), ImVec2(c.x, c.y + g + L), col, w);
        dl->AddCircleFilled(c, (pass ? 2.2f : 3.6f) * k, col);
    }
}
// MATRIX tint (render thread): a dark green wash over the whole picture + a vignette. Behind the panel and the rain.
static void tint_draw() {
    if (!dk::g_rain.tint.load() || !ImGui::GetCurrentContext()) return;
    const ImVec2 D = ImGui::GetIO().DisplaySize; if (D.x < 64 || D.y < 64) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    dl->AddRectFilled(ImVec2(0, 0), D, IM_COL32(0, 70, 20, 80));
    const ImU32 edge = IM_COL32(0, 8, 2, 150), none = IM_COL32(0, 8, 2, 0);
    const float bx = D.x * 0.22f, by = D.y * 0.22f;
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(D.x, by), edge, edge, none, none);                // top
    dl->AddRectFilledMultiColor(ImVec2(0, D.y - by), D, none, none, edge, edge);                       // bottom
    dl->AddRectFilledMultiColor(ImVec2(0, 0), ImVec2(bx, D.y), edge, none, none, edge);                // left
    dl->AddRectFilledMultiColor(ImVec2(D.x - bx, 0), D, none, edge, edge, none);                       // right
}
void hud_draw() {
    try { tp_crosshair_draw(); } catch (...) {}
    try { tint_draw(); } catch (...) {}
    try { rain_draw(); } catch (...) {}
    std::vector<dk::HudMark> marks;
    { std::lock_guard<std::mutex> lk(dk::g_hud_mtx); if (GetTickCount64() - dk::g_hud_at > 1000) return; marks = dk::g_hud_marks; }
    if (marks.empty() || !ImGui::GetCurrentContext()) return;
    ImDrawList* dl = ImGui::GetBackgroundDrawList();
    for (auto& m : marks) {
        const ImVec2 ts = ImGui::CalcTextSize(m.text.c_str());
        const ImVec2 p(m.x - ts.x * 0.5f, m.y - ts.y);
        dl->AddRectFilled(ImVec2(p.x - 4, p.y - 2), ImVec2(p.x + ts.x + 4, p.y + ts.y + 2), IM_COL32(0, 0, 0, 150), 3.f);
        dl->AddText(p, m.color, m.text.c_str());
    }
}
} // namespace wb
