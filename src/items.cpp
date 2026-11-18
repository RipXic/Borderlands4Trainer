// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// Works on item serials (serial.cpp) with the part catalog the trainer sweeps from the game (BL.ITEMDB -> bin\itemdb.json):
// every part of every item type with its serial index, mesh (gestalt) names and stat description ("Damage mod").
// Edits produce a new serial; it reaches the game through the save editor queue (savefile.cpp: add / replace), which the
// next game start applies (the game only reads items from the save when it starts).
#include "common.h"
#include "serial.h"
#include <fstream>
#include <map>
#include <cmath>
#include <set>
#include <mutex>
#include <thread>
#include <memory>
#include <filesystem>
#include "imgui.h"
#include "ui_kit.h"

namespace dk { json cmd(const std::string& name, const json& a); }

namespace wb::items {
using serial::Item; using serial::Tok;

// ------------------------------------------------------------------ catalog
struct Part { int index = -1; std::string gestalt, desc, slot, gslot, name, addr; bool mod = false; };   // name = the game's part name (aspect)   // gslot = the game's slot name (itemdb)
struct Type { int index = -1; std::string name, maker; std::vector<Part> parts; };
static std::mutex g_db_mtx;
// The catalog by serial index. It is built aside and swapped in whole under g_db_mtx; earlier catalogs are kept alive (a
static std::vector<std::shared_ptr<const std::map<int, Type>>> g_types_keep;
static const std::map<int, Type> g_types_none{};
static const std::map<int, Type>* g_types_cur = &g_types_none;   // guarded by g_db_mtx
static ULONGLONG g_db_checked = 0; static long long g_db_mtime = 0;   // itemdb.json time stamp (a rebuild by BL.ITEMDB is picked up)
static const std::map<int, Type>& types_now() { std::lock_guard<std::mutex> lk(g_db_mtx); return *g_types_cur; }
// Background work: a detached thread must never let an exception escape (that ends the game). on_fail resets the busy flag.
template <class F, class G> static void bg_run(F f, G on_fail) {
    std::thread([f, on_fail]() { try { f(); } catch (...) { try { on_fail(); } catch (...) {} } }).detach();
}
static bool g_db_loaded = false; static std::string g_db_msg;

static void slot_of(Part& p, const Type& t) {
    std::vector<std::string> tk; { std::string cur; for (char c : p.gestalt) { if (c == '_') { tk.push_back(cur); cur.clear(); } else cur += c; } tk.push_back(cur); }
    size_t i = 1;                                                      // drop the maker (DAD / GRN / SHD ...)
    if (t.name.rfind("Weapon_", 0) == 0 && tk.size() > 2 && tk[1].size() == 2) i = 2;   // and the weapon type (SM / AR ...)
    if (p.gestalt.empty() || i >= tk.size()) { p.slot = "Other"; return; }
    p.slot = tk[i];   // "Body_Mag" -> "Body Mag", "Barrel_01_A" -> "Barrel" (mod), "Scope_01_L1" -> "Scope"
    for (size_t k = i + 1; k < tk.size(); ++k) {
        const std::string& x = tk[k];
        if (x.size() == 1 && isupper((unsigned char)x[0])) { p.mod = true; continue; }
        if (!x.empty() && !isdigit((unsigned char)x[0]) && x[0] != 'L' && p.slot.find(' ') == std::string::npos && k == i + 1) p.slot += " " + x;
    }
}
static long long db_file_time(const std::string& path) {
    std::error_code ec; const auto t = std::filesystem::last_write_time(std::filesystem::path(path), ec);
    return ec ? 0 : (long long)t.time_since_epoch().count();
}
static bool load_db() {
    std::lock_guard<std::mutex> lk(g_db_mtx);
    const std::string path = g_cfg.root + "\\itemdb.json";
    if (g_db_loaded && GetTickCount64() - g_db_checked > 2000) {   // rebuilt meanwhile (BL.ITEMDB)? then read it again
        g_db_checked = GetTickCount64();
        if (db_file_time(path) != g_db_mtime) g_db_loaded = false;
    }
    if (g_db_loaded) return !g_types_cur->empty();
    g_db_loaded = true; g_db_checked = GetTickCount64(); g_db_mtime = db_file_time(path);
    auto fresh = std::make_shared<std::map<int, Type>>();
    auto publish = [&]() { if (fresh->empty()) g_types_cur = &g_types_none; else { g_types_keep.push_back(fresh); g_types_cur = fresh.get(); } return !fresh->empty(); };
    std::ifstream f(path); if (!f) { g_db_msg = "no item database yet - press 'Build item database now' (needs a weapon in the backpack)"; return publish(); }
    json j = json::parse(f, nullptr, false); if (!j.is_object()) { g_db_msg = "itemdb.json unreadable"; return publish(); }
    try {   // one entry of the wrong type must not throw into the panel: the catalog is then empty and can be built again
        for (auto& [k, t] : j["types"].items()) {
            const int idx = t.value("index", -1); if (idx < 0) continue;
            Type ty; ty.index = idx; ty.name = t.value("name", ""); ty.maker = t.value("maker", "");
            for (auto& p : t["parts"]) { Part pp; pp.index = p.value("index", -1); pp.desc = p.value("desc", ""); pp.gslot = p.value("slot", ""); pp.name = p.value("name", ""); pp.addr = p.value("addr", "");
                if (p.contains("gestalt") && !p["gestalt"].empty()) pp.gestalt = p["gestalt"][0].get<std::string>(); slot_of(pp, ty); ty.parts.push_back(pp); }
            std::sort(ty.parts.begin(), ty.parts.end(), [](const Part& a, const Part& b) { return a.slot != b.slot ? a.slot < b.slot : a.index < b.index; });
            auto it = fresh->find(idx);
            if (it == fresh->end() || it->second.parts.size() < ty.parts.size()) (*fresh)[idx] = ty;   // the richest type of that index
        }
    } catch (std::exception& ex) { fresh->clear(); g_db_msg = std::string("itemdb.json has a bad entry (") + ex.what() + ") - build it again"; return publish(); }
    g_db_msg = std::to_string(fresh->size()) + " item types";
    return publish();
}
void reload_db() { std::lock_guard<std::mutex> lk(g_db_mtx); g_db_loaded = false; }
static const Type* type_of(int idx) { load_db(); const auto& m = types_now(); auto it = m.find(idx); return it == m.end() ? nullptr : &it->second; }
static const Part* part_of(const Type* t, int idx) { if (!t) return nullptr; for (auto& p : t->parts) if (p.index == idx) return &p; return nullptr; }
static std::string nice_name(const std::string& n) {   // "JAK_AR.part_mag_ted_thrown_01" -> "mag ted thrown 01"
    std::string s = n; const size_t d = s.find('.'); if (d != std::string::npos) s = s.substr(d + 1);
    if (s.rfind("part_", 0) == 0) s = s.substr(5);
    for (auto& c : s) if (c == '_') c = ' ';
    static const std::pair<const char*, const char*> mk[] = { {" tor", " Torgue"}, {" cov", " COV"}, {" borg", " Ripper"}, {" bor", " Ripper"},
        {" ted", " Tediore"}, {" hyp", " Hyperion"}, {" jak", " Jakobs"}, {" mal", " Maliwan"}, {" vla", " Vladof"}, {" dad", " Daedalus"} };
    s += " ";
    for (auto& [a, r] : mk) { const std::string k = std::string(a) + " "; size_t at; while ((at = s.find(k)) != std::string::npos) s.replace(at, k.size(), std::string(r) + " "); }
    s.pop_back();
    return s;
}
static std::string part_label(const Part* p, int idx) {
    if (!p) return "part #" + std::to_string(idx);
    if (!p->name.empty()) { /* the game's part name says what it does: "mag 03 Torgue", "barrel 01 DualDamage" */ const std::string g = nice_name(p->name) + "  #" + std::to_string(idx); return p->desc.empty() ? g : g + "  (" + p->desc + ")"; }
    std::string g = p->gestalt.empty() ? "part #" + std::to_string(idx) : p->gestalt + "  #" + std::to_string(idx);
    return p->desc.empty() ? g : g + "  (" + p->desc + ")";
}
static const char* kRarity[] = { "Common", "Uncommon", "Rare", "Epic" };   // weapon comps 95..98
static std::string pretty(const Type* t, int ti) {   // "Daedalus SMG", "Shield", "Grenade"
    if (!t) return "type " + std::to_string(ti);
    static const std::map<std::string, std::string> mk = { {"BOR", "Ripper"}, {"DAD", "Daedalus"}, {"JAK", "Jakobs"}, {"MAL", "Maliwan"},
        {"ORD", "Order"}, {"TED", "Tediore"}, {"TOR", "Torgue"}, {"VLA", "Vladof"} };
    static const std::map<std::string, std::string> ty = { {"Weapon_PS", "Pistol"}, {"Weapon_SG", "Shotgun"}, {"Weapon_AR", "Assault Rifle"},
        {"Weapon_SM", "SMG"}, {"Weapon_SR", "Sniper"}, {"energy_shield", "Shield"}, {"armor_shield", "Armor shield"}, {"grenade_gadget", "Grenade"},
        {"heavy_weapon_gadget", "Heavy weapon"}, {"repair_kit", "Repair kit"}, {"Enhancement", "Enhancement"}, {"turret_gadget", "Turret"} };
    auto m = mk.find(t->maker); auto y = ty.find(t->name);
    std::string tn = y != ty.end() ? y->second : t->name;
    return m != mk.end() ? m->second + " " + tn : tn;
}

// ------------------------------------------------------------------ decoded view
static json view(const Item& it) {
    json o = { {"text", serial::text(it)}, {"serial", serial::encode(it)} };
    const int ti = serial::type_index(it); const Type* t = type_of(ti);
    o["type"] = { {"index", ti}, {"name", t ? t->name : ""}, {"maker", t ? t->maker : ""} };
    o["level"] = serial::level(it);
    json parts = json::array(); const int cp = serial::comp_part(it);
    for (size_t i = 0; i < it.toks.size(); ++i) {
        const Tok& k = it.toks[i]; if (k.kind != Tok::Part) continue;
        json p = { {"tok", (int)i}, {"index", (int)k.idx.v} };
        if ((int)i == cp) { const int c = (int)k.idx.v; p["role"] = "rarity"; p["label"] = c >= 95 && c <= 98 ? kRarity[c - 95] : "special (legendary / pearl) #" + std::to_string(c); parts.push_back(p); continue; }
        if (k.sub == Tok::Plain) { const Part* pp = part_of(t, (int)k.idx.v); p["label"] = part_label(pp, (int)k.idx.v); p["slot"] = pp ? pp->slot : "Other"; p["mod"] = pp && pp->mod; }
        else {   // shared category: {cat:v} / {cat:[v ...]}
            const Type* ct = type_of((int)k.idx.v); p["shared"] = true; json vals = json::array();
            auto one = [&](const serial::Val& v) { const Part* pp = part_of(ct, (int)v.v); vals.push_back({ {"index", (int)v.v}, {"label", part_label(pp, (int)v.v)} }); };
            if (k.sub == Tok::One) one(k.one); else for (auto& v : k.list) if (v.kind != serial::Val::Soft) one(v);
            p["values"] = vals; p["label"] = (ct ? ct->name + (ct->maker.empty() ? "" : " " + ct->maker) : "shared " + std::to_string(k.idx.v));
        }
        parts.push_back(p);
    }
    o["parts"] = parts;
    return o;
}

// {serial, level, rarity 0..3, set:[{tok, index}], remove:[tok], add:[index]} -> new serial
static bool apply_edit(Item& it, const json& a, std::string& err) {
    if (a.contains("level") && !serial::set_level(it, a["level"].get<int>())) { err = "no level field"; return false; }
    if (a.contains("rarity")) {
        const int r = a["rarity"].get<int>(); const int fp = serial::comp_part(it);
        if (fp < 0) { err = "this item has no rarity part to change (its comp is written by name)"; return false; }
        if (r < 0 || r > 3) { err = "rarity"; return false; }
        it.toks[fp].idx.v = 95 + r; it.toks[fp].sub = Tok::Plain;
    }
    for (auto& s : a.value("set", json::array())) {
        const int ti = s.value("tok", -1), v = s.value("index", -1);
        if (ti < 0 || ti >= (int)it.toks.size() || it.toks[ti].kind != Tok::Part || v < 0) { err = "set: bad token"; return false; }
        it.toks[ti].idx.v = v;
    }
    std::vector<int> rm; for (auto& r : a.value("remove", json::array())) rm.push_back(r.get<int>());
    std::sort(rm.rbegin(), rm.rend());
    for (int r : rm) if (r >= 0 && r < (int)it.toks.size() && it.toks[r].kind == Tok::Part) it.toks.erase(it.toks.begin() + r);
    for (auto& ad : a.value("add", json::array())) { Tok t; t.kind = Tok::Part; t.sub = Tok::Plain; t.idx.v = ad.get<int>();
        int at = (int)it.toks.size(); for (int q = (int)it.toks.size() - 1; q >= 0; --q) if (it.toks[q].kind == Tok::Part) { at = q + 1; break; }   // after the last part, never behind a trailing section
        it.toks.insert(it.toks.begin() + at, t); }
    return true;
}

static json legendary_cmd(const json& a);
json cmd(const std::string& name, const json& a) {
    if (name == "TQ.ITEM_LEGENDARY") return legendary_cmd(a);   // {type, comp | name, level} -> serial (also opened in Gear Maker)
    if (name == "TQ.ITEM_DECODE" || name == "TQ.ITEM_ENCODE") {
        Item it; std::string err;
        if (!serial::decode(a.value("serial", ""), it, &err)) return fail("SERIAL", err);
        if (name == "TQ.ITEM_ENCODE" && !apply_edit(it, a, err)) return fail("EDIT", err);
        json v = view(it);
        Item back; if (!serial::decode(v["serial"], back) || serial::encode(back) != v["serial"]) return fail("ROUNDTRIP", "the new serial does not decode back the same - not used");
        return ok(v, v["type"].value("maker", std::string()) + " " + v["type"].value("name", std::string()) + " level " + std::to_string(v.value("level", 0)));
    }
    if (name == "TQ.ITEM_OPTIONS") {
        const Type* t = type_of(a.value("type", -1)); if (!t) return fail("TYPE", g_db_msg.empty() ? "unknown type" : g_db_msg);
        json parts = json::array(); for (auto& p : t->parts) parts.push_back({ {"index", p.index}, {"slot", p.slot}, {"mod", p.mod}, {"label", part_label(&p, p.index)} });
        return ok({ {"name", t->name}, {"maker", t->maker}, {"parts", parts} });
    }
    if (name == "TQ.ITEM_TEST") {   // decode + encode every serial given (or in the backpack) -> how many round trip exactly
        json ss = a.value("serials", json::array()); int okc = 0, bad = 0; json fails = json::array();
        for (auto& s : ss) { Item it; std::string e; if (serial::decode(s.get<std::string>(), it, &e) && serial::encode(it) == s.get<std::string>()) ++okc; else { ++bad; fails.push_back(s); } }
        return ok({ {"ok", okc}, {"bad", bad}, {"fails", fails} });
    }
    return fail("UNKNOWN", name);
}

// ------------------------------------------------------------------ the ITEM EDITOR page (panel)
struct Row { int slot; std::string where, serial, title; };
static std::mutex g_mtx;
static std::vector<Row> g_rows; static std::string g_list_msg; static bool g_busy = false;
static int g_sel = -1; static char g_paste[512] = {};
static Item g_item; static bool g_have = false; static std::string g_orig, g_edit_msg; static bool g_edit_ok = true;
static std::string g_src; static bool g_src_bag = false;   // the serial that was opened and whether it came from the backpack (Reset)
static json g_view;

static std::string title_of(const std::string& s) {
    Item it; if (!serial::decode(s, it)) return "(unreadable)";
    const Type* t = type_of(serial::type_index(it)); const int fp = serial::comp_part(it); std::string r;
    if (fp >= 0) { const int c = (int)it.toks[fp].idx.v; r = c >= 95 && c <= 98 ? kRarity[c - 95] : "Special"; }
    if (!t || t->name.rfind("Weapon_", 0) != 0) r.clear();   // gear rarity is not one of the 4 weapon comps
    return pretty(t, serial::type_index(it)) + (serial::level(it) >= 0 ? "   level " + std::to_string(serial::level(it)) : std::string()) + (r.empty() ? "" : "   " + r);
}
static void refresh_list() {
    if (g_busy) return; g_busy = true; g_list_msg = "reading...";
    bg_run([] {
        std::vector<Row> rows; std::string msg;
        for (const char* where : { "backpack", "ground" }) {
            json r = dk::cmd("BL.SERIALS", { {"where", where} });
            if (!r.value("ok", false)) { msg = r.value("message", std::string("not in game")); continue; }
            for (auto& e : r["data"].value(where, json::array())) { const std::string s = e.value("serial", ""); if (s.empty()) continue;
                rows.push_back({ e.value("slot", -1), where, s, title_of(s) }); }
        }
        std::lock_guard<std::mutex> lk(g_mtx); g_rows = rows; g_list_msg = msg.empty() ? std::to_string(rows.size()) + " items" : msg; g_busy = false;
        g_sel = -1;   // the rows moved: the selection follows the opened item (or is cleared)
        if (g_have) for (int i = 0; i < (int)g_rows.size(); ++i) if (g_rows[i].serial == g_src && (g_rows[i].where == "backpack") == g_src_bag) { g_sel = i; break; }
    }, [] { std::lock_guard<std::mutex> lk(g_mtx); g_busy = false; g_list_msg = "could not read the items"; });
}
static void open_serial(const std::string& s, bool from_bag) {
    Item it; std::string err;
    if (!serial::decode(s, it, &err)) { g_have = false; g_edit_msg = "Not a serial: " + err; g_edit_ok = false; return; }
    { const std::string src = s; g_item = it; g_have = true; g_orig = from_bag ? src : std::string(); g_src = src; g_src_bag = from_bag; }   // (s may be g_src itself)
    g_view = view(it); g_edit_msg.clear();
}
static void edited() { g_view = view(g_item); }
static void queue(const json& args, const std::string& done) {
    json r = dk::cmd("BL.SAVE_QUEUE", args);
    g_edit_ok = r.value("ok", false); g_edit_msg = g_edit_ok ? done + " - " + r.value("message", std::string()) : r.value("message", std::string("failed"));
}

void draw_page(float S) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!load_db()) {
        ImGui::TextWrapped("%s", g_db_msg.c_str());
        if (ImGui::Button(g_busy ? "Building..." : "Build item database now") && !g_busy) {
            g_busy = true;
            bg_run([] { json r = dk::cmd("BL.ITEMDB", json::object()); reload_db(); std::lock_guard<std::mutex> lk2(g_mtx); g_busy = false;
                             g_db_msg = r.value("ok", false) ? "built" : r.value("message", std::string("failed")); },
                  [] { std::lock_guard<std::mutex> lk2(g_mtx); g_busy = false; });
        }
        return;
    }
    const float w = ImGui::GetContentRegionAvail().x;
    // ---- source
    ImGui::TextDisabled("Pick an item from your backpack / the ground, or paste a serial (they start with @U).");
    if (ImGui::Button(g_busy ? "Reading..." : "Refresh list")) refresh_list();
    ImGui::SameLine(); ImGui::TextDisabled("%s", g_list_msg.c_str());
    if (g_rows.empty() && !g_busy && g_list_msg.empty()) refresh_list();
    ImGui::BeginChild("##items_list", ImVec2(w, S * 150), ImGuiChildFlags_Borders);
    for (int i = 0; i < (int)g_rows.size(); ++i) {
        const Row& r = g_rows[i]; char lab[256]; snprintf(lab, sizeof lab, "%s %d   %s##r%d", r.where == "ground" ? "Ground" : "Bag", r.slot, r.title.c_str(), i);
        if (ImGui::Selectable(lab, g_sel == i)) { g_sel = i; open_serial(r.serial, r.where == "backpack"); }
    }
    ImGui::EndChild();
    ImGui::SetNextItemWidth(w - S * 110); ImGui::InputTextWithHint("##paste", "@Ug...", g_paste, sizeof g_paste); ImGui::SameLine();
    if (ImGui::Button("Open##paste", ImVec2(S * 100, 0))) { g_sel = -1; open_serial(g_paste, false); }
    if (!g_have) { if (!g_edit_msg.empty()) ImGui::TextColored(ImVec4(1, .4f, .35f, 1), "%s", g_edit_msg.c_str()); return; }

    // ---- editor
    ImGui::Separator();
    const int ti = serial::type_index(g_item); const Type* t = type_of(ti);
    ImGui::Text("%s", pretty(t, ti).c_str());
    int lv = serial::level(g_item);
    if (lv >= 0) { ImGui::SetNextItemWidth(S * 160); if (ImGui::InputInt("Level", &lv)) { serial::set_level(g_item, lv); edited(); } }
    else ImGui::TextDisabled("Level: this item has no level field");
    {   const int fp = serial::comp_part(g_item);   // -1: the comp is written by name - no rarity to pick
        if (fp >= 0) { const int c = (int)g_item.toks[fp].idx.v; int r = c >= 95 && c <= 98 ? c - 95 : -1;
            const bool weapon = t && t->name.rfind("Weapon_", 0) == 0;
            if (weapon) { ImGui::SameLine(); ImGui::SetNextItemWidth(S * 180);
                if (ImGui::BeginCombo("Rarity", r >= 0 ? kRarity[r] : "Special (keep)")) {
                    for (int k = 0; k < 4; ++k) if (ImGui::Selectable(kRarity[k], r == k)) { g_item.toks[fp].idx.v = 95 + k; g_item.toks[fp].sub = Tok::Plain; edited(); }
                    ImGui::EndCombo(); } }
        } }
    ImGui::TextDisabled("Parts - pick another part of the same slot, x removes it");
    int remove_tok = -1;
    const int comp_tok = serial::comp_part(g_item);
    for (size_t i = 0; i < g_item.toks.size(); ++i) {
        Tok& k = g_item.toks[i]; if (k.kind != Tok::Part) continue;
        if ((int)i == comp_tok) continue;   // rarity comp (above)
        ImGui::PushID((int)i);
        if (k.sub != Tok::Plain) {
            const Type* ct = type_of((int)k.idx.v);
            std::string s = (ct ? ct->name : "shared") + ": ";
            auto add = [&](const serial::Val& v) { s += part_label(part_of(ct, (int)v.v), (int)v.v) + "  "; };
            if (k.sub == Tok::One) add(k.one); else for (auto& v : k.list) if (v.kind != serial::Val::Soft) add(v);
            ImGui::TextDisabled("%s", s.c_str());
        } else {
            const Part* cur = part_of(t, (int)k.idx.v);
            ImGui::SetNextItemWidth(w - S * 60);
            if (ImGui::BeginCombo("##p", part_label(cur, (int)k.idx.v).c_str(), ImGuiComboFlags_HeightLarge)) {
                if (t) for (auto& p : t->parts) {
                    if (cur && (p.slot != cur->slot || p.mod != cur->mod)) continue;
                    if (ImGui::Selectable((part_label(&p, p.index) + "##o" + std::to_string(p.index)).c_str(), cur && p.index == cur->index)) { k.idx.v = p.index; edited(); }
                }
                ImGui::EndCombo();
            }
            ImGui::SameLine(); if (ImGui::Button("x", ImVec2(S * 40, 0))) remove_tok = (int)i;
        }
        ImGui::PopID();
    }
    if (remove_tok >= 0) { g_item.toks.erase(g_item.toks.begin() + remove_tok); edited(); }
    ImGui::SetNextItemWidth(w);
    if (t && ImGui::BeginCombo("##add", "+ add a part...", ImGuiComboFlags_HeightLarge)) {
        std::string last;
        for (auto& p : t->parts) { if (p.slot != last) { ImGui::TextDisabled("%s", p.slot.c_str()); last = p.slot; }
            if (ImGui::Selectable(("  " + part_label(&p, p.index) + "##a" + std::to_string(p.index)).c_str())) {
                Tok nt; nt.kind = Tok::Part; nt.sub = Tok::Plain; nt.idx.v = p.index;
                int at = (int)g_item.toks.size(); for (int q = (int)g_item.toks.size() - 1; q >= 0; --q) if (g_item.toks[q].kind == Tok::Part && g_item.toks[q].sub == Tok::Plain) { at = q + 1; break; }
                g_item.toks.insert(g_item.toks.begin() + at, nt); edited(); } }
        ImGui::EndCombo();
    }
    // ---- output
    const std::string ns = serial::encode(g_item);
    ImGui::Separator();
    ImGui::TextDisabled("New serial");
    ImGui::TextWrapped("%s", ns.c_str());
    if (ImGui::Button("Copy serial")) ImGui::SetClipboardText(ns.c_str());
    ImGui::SameLine(); if (ImGui::Button("Add to backpack (after restart)")) queue({ {"serial", ns} }, "queued as a new item");
    if (!g_orig.empty()) { ImGui::SameLine(); if (ImGui::Button("Replace the original (queue)")) queue({ {"op", "replace_item"}, {"old", g_orig}, {"new", ns} }, "queued: replace"); }
    ImGui::SameLine(); if (ImGui::Button("Reset")) { const std::string src = g_src; open_serial(src, g_src_bag); }
    if (!g_edit_msg.empty()) ImGui::TextColored(g_edit_ok ? ImVec4(.6f, .9f, .45f, 1) : ImVec4(1, .4f, .35f, 1), "%s", g_edit_msg.c_str());
    ImGui::TextDisabled("Queued items arrive in your backpack the next time the game starts (Money tab: Apply & restart).");
}

// ------------------------------------------------------------------ the MISSIONS page (read-only log + go to objective markers)
// Never changes mission data: the log is read from the save, markers from the live game; "Go" only teleports the player.
// Objectives are then finished by playing (with the trainer's kill / vacuum tools) and the game records them itself.
static std::mutex g_ms_mtx; static json g_marks = json::array(), g_log = json::object(); static std::string g_ms_msg; static bool g_ms_busy = false;
static std::string nice_mission(std::string m) {   // "zoneactivity_grasslands_safehouse3" -> "Grasslands safehouse 3"
    for (const char* p : { "zoneactivity_", "missionset_", "mission_" }) if (m.rfind(p, 0) == 0) m = m.substr(strlen(p));
    for (auto& c : m) if (c == '_') c = ' ';
    std::string o; for (size_t i = 0; i < m.size(); ++i) { if (i && isdigit((unsigned char)m[i]) && isalpha((unsigned char)m[i - 1])) o += ' '; o += m[i]; }
    if (!o.empty()) o[0] = (char)toupper((unsigned char)o[0]);
    return o;
}
static void ms_refresh() {
    if (g_ms_busy) return; g_ms_busy = true;
    bg_run([] {
        json m = dk::cmd("BL.MISSIONS", json::object()), l = dk::cmd("BL.SAVE_MISSIONS", json::object());
        std::lock_guard<std::mutex> lk(g_ms_mtx);
        g_marks = m.value("ok", false) ? m["data"].value("markers", json::array()) : json::array();
        g_log = l.value("ok", false) ? l["data"] : json::object();
        g_ms_msg = m.value("ok", false) ? m.value("message", std::string()) : m.value("message", std::string("not in game"));
        g_ms_busy = false;
    }, [] { std::lock_guard<std::mutex> lk(g_ms_mtx); g_ms_busy = false; g_ms_msg = "could not read the missions"; });
}
static void ms_go(const json& args) {
    if (g_ms_busy) { g_ms_msg = "busy - try again in a moment"; return; } g_ms_busy = true;
    bg_run([args] { json r = dk::cmd("BL.MISSIONS", args); std::lock_guard<std::mutex> lk(g_ms_mtx); g_ms_msg = r.value("message", std::string()); g_ms_busy = false; },
          [] { std::lock_guard<std::mutex> lk(g_ms_mtx); g_ms_busy = false; g_ms_msg = "failed"; });
}
void draw_missions_page(float S) {
    std::lock_guard<std::mutex> lk(g_ms_mtx);
    static bool first = true; if (first) { first = false; g_ms_busy = false; ms_refresh(); }
    ImGui::TextDisabled("Read-only: the trainer never changes mission progress. Go takes you to an objective marker - finish it yourself.");
    if (ImGui::Button(g_ms_busy ? "Working..." : "Refresh")) ms_refresh();
    ImGui::SameLine(); if (ImGui::Button("Go to nearest marker")) ms_go({ {"v", "nearest"} });
    ImGui::SameLine(); if (ImGui::Button("Nearest side mission")) ms_go({ {"v", "Secondary"} });
    ImGui::SameLine(); ImGui::TextDisabled("%s", g_ms_msg.c_str());
    ImGui::Separator();
    ImGui::Text("Objective markers");
    for (auto& m : g_marks) {
        const int i = m.value("i", -1); ImGui::PushID(i);
        std::string cat = m.value("category", std::string("?")); if (cat == "Secondary") cat = "Side"; else if (cat == "None" || cat == "?") cat = "Mission";
        ImGui::Text("%-8s  %6.0f m", cat.c_str(), m.value("dist_m", 0.0));
        ImGui::SameLine(S * 220); if (ImGui::SmallButton("Go")) ms_go({ {"go", i} });
        ImGui::PopID();
    }
    if (g_marks.empty()) ImGui::TextDisabled("No markers (load into the game, or track a mission on the map).");
    ImGui::Separator();
    ImGui::Text("Mission log  (from the save - updates when the game saves)");
    for (auto& t : g_log.value("tracked", json::array())) ImGui::TextColored(ImVec4(1, .8f, .25f, 1), "Tracked: %s", nice_mission(t.get<std::string>()).c_str());
    for (auto& set : g_log.value("sets", json::array())) {
        int act = 0, done = 0; for (auto& m : set["missions"]) (m.value("status", std::string()) == "completed" ? done : act)++;
        char h[256]; snprintf(h, sizeof h, "%s   (%d active, %d done)##%s", nice_mission(set.value("set", std::string())).c_str(), act, done, set.value("set", std::string()).c_str());
        if (ImGui::TreeNode(h)) {
            for (auto& m : set["missions"]) { const std::string st = m.value("status", std::string());
                ImGui::TextColored(st == "completed" ? ImVec4(.55f, .8f, .45f, 1) : st == "Active" ? ImVec4(1, .85f, .4f, 1) : ImVec4(.7f, .7f, .7f, 1), "%s  -  %s",
                                   nice_mission(m.value("mission", std::string())).c_str(), st.empty() ? "-" : st.c_str()); }
            ImGui::TreePop();
        }
    }
}

// ------------------------------------------------------------------ ENEMIES page: nearby enemies, crosshair / area actions,
// world spawn swap, spawn anything from the 1883-character catalog with options (bl4.cpp BL.ENEMY*, BL.SPAWN_SWAP)
static std::mutex g_en_mtx;
static json g_en_list = json::array(), g_en_swap = json::object();
static std::shared_ptr<const json> g_en_cat = std::make_shared<const json>(json::array());   // swapped whole, never edited
static std::string g_en_msg; static bool g_en_busy = false; static ULONGLONG g_en_at = 0;
static std::string pretty_char(std::string n) {   // "Char_GunToterAssault_Female" -> "Gun Toter Assault (F)"
    if (n.rfind("Char_", 0) == 0) n = n.substr(5);
    std::string sx; if (n.size() > 7 && n.compare(n.size() - 7, 7, "_Female") == 0) { n.resize(n.size() - 7); sx = " (F)"; }
    else if (n.size() > 5 && n.compare(n.size() - 5, 5, "_Male") == 0) { n.resize(n.size() - 5); sx = " (M)"; }
    std::string o; for (size_t i = 0; i < n.size(); ++i) { const char c = n[i]; if (c == '_') { o += ' '; continue; }
        if (i && isupper((unsigned char)c) && islower((unsigned char)n[i - 1])) o += ' '; o += c; }
    return o + sx;
}
// One action can wait while a list refresh / another action runs (before, a click in that moment was dropped silently).
static bool g_en_has_next = false; static std::string g_en_next_cmd; static json g_en_next_args; static bool g_en_next_refresh = true;
static void en_call(const std::string& cmdname, const json& args, bool refresh_after = true);
static void en_run_next() {
    std::string c; json a; bool r = true;
    { std::lock_guard<std::mutex> lk(g_en_mtx); if (!g_en_has_next || g_en_busy) return; c = g_en_next_cmd; a = g_en_next_args; r = g_en_next_refresh; g_en_has_next = false; }
    en_call(c, a, r);
}
static void en_call(const std::string& cmdname, const json& args, bool refresh_after) {
    { std::lock_guard<std::mutex> lk(g_en_mtx);
      if (g_en_busy) { g_en_has_next = true; g_en_next_cmd = cmdname; g_en_next_args = args; g_en_next_refresh = refresh_after; return; }
      g_en_busy = true; }
    bg_run([cmdname, args, refresh_after] {
        json r = dk::cmd(cmdname, args);
        std::string m;
        if (r.value("ok", false)) { m = cmdname.substr(3) + ": "; m += (r["data"].is_object() && r["data"].contains("done")) ? std::to_string(r["data"].value("done", 0)) + " done" : std::string("ok"); }
        else m = r.value("message", std::string("failed"));
        json l = refresh_after ? dk::cmd("BL.ENEMY_LIST", { {"radius_m", 150}, {"max", 60} }) : json();
        {
            std::lock_guard<std::mutex> lk(g_en_mtx);
            if (cmdname == "BL.SPAWN_SWAP" && r.value("ok", false)) g_en_swap = r["data"];
            if (refresh_after && l.is_object() && l.value("ok", false)) g_en_list = l["data"].value("enemies", json::array());
            g_en_msg = m; g_en_busy = false;
        }
        en_run_next();
    }, [] { { std::lock_guard<std::mutex> lk(g_en_mtx); g_en_busy = false; g_en_msg = "failed"; } en_run_next(); });
}
static void en_refresh() {
    { std::lock_guard<std::mutex> lk(g_en_mtx); if (g_en_busy) return; g_en_busy = true; g_en_at = GetTickCount64(); }
    bg_run([] {
        json l = dk::cmd("BL.ENEMY_LIST", { {"radius_m", 150}, {"max", 60} }), sw = dk::cmd("BL.SPAWN_SWAP", json::object());
        json c; bool need_cat; { std::lock_guard<std::mutex> lk(g_en_mtx); need_cat = g_en_cat->empty(); }
        if (need_cat) c = dk::cmd("BL.ENEMY_CATALOG", { {"max", 4000} });
        {
            std::lock_guard<std::mutex> lk(g_en_mtx);
            if (l.value("ok", false)) g_en_list = l["data"].value("enemies", json::array()); else g_en_msg = l.value("message", std::string("not in game"));
            if (sw.value("ok", false)) g_en_swap = sw["data"];
            if (need_cat && c.is_object() && c.value("ok", false)) g_en_cat = std::make_shared<const json>(c["data"].value("enemies", json::array()));
            g_en_busy = false;
        }
        en_run_next();
    }, [] { { std::lock_guard<std::mutex> lk(g_en_mtx); g_en_busy = false; } en_run_next(); });
}
void draw_enemies_page(float S) {
    static bool first = true; if (first) { first = false; { std::lock_guard<std::mutex> lk(g_en_mtx); g_en_busy = false; } en_refresh(); }
    if (GetTickCount64() - g_en_at > 3000) en_refresh();   // live list
    json en_list, en_swap; std::string en_msg; bool en_busy; std::shared_ptr<const json> en_cat;
    { std::lock_guard<std::mutex> lk(g_en_mtx); en_list = g_en_list; en_swap = g_en_swap; en_msg = g_en_msg; en_busy = g_en_busy; en_cat = g_en_cat; }
    static const char* kRanks[] = { "normal", "elite", "badass", "super", "ultimate", "mini", "boss" };
    static const char* kRankNames[] = { "Normal", "Elite", "Badass", "Super Badass", "Ultimate Badass", "Mini boss", "Boss" };
    const ImVec4 gold(1, .82f, .2f, 1);

    // ---- crosshair / area
    ImGui::TextColored(gold, "AIM AT AN ENEMY");
    ImGui::SameLine(); ImGui::TextDisabled("(the one closest to your crosshair, up to 150 m)");
    auto act = [&](const char* label, const char* target, const char* action, json v, double radius) {
        if (ImGui::Button(label)) { json a = { {"target", target}, {"action", action} }; if (!v.is_null()) a["v"] = v; if (radius > 0) a["radius_m"] = radius; en_call("BL.ENEMY", a); }
        ImGui::SameLine();
    };
    ImGui::PushID("aim");
    act("Kill", "aim", "kill", json(), 0); act("Make ally", "aim", "ally", json(), 0); act("Freeze", "aim", "freeze", json(), 0); act("Unfreeze", "aim", "unfreeze", json(), 0);
    act("Giant x2.5", "aim", "scale", 2.5, 0); act("Tiny x0.4", "aim", "scale", 0.4, 0); ImGui::NewLine();
    act("Arm with a gun from my backpack", "aim", "weapon", -1, 0); act("Ultimate Badass", "aim", "rank", "ultimate", 0);
    act("Health x5", "aim", "health", 5, 0); act("Bring here", "aim", "bring", json(), 0); act("Yeet", "aim", "launch", 3000, 0);
    ImGui::NewLine(); ImGui::PopID();
    static float rad = 60;
    ImGui::TextColored(gold, "EVERY ENEMY AROUND YOU"); ImGui::SameLine(); ImGui::SetNextItemWidth(S * 220); ImGui::SliderFloat("##rad", &rad, 10, 300, "%.0f m");
    ImGui::PushID("all");
    act("Kill all", "all", "kill", json(), rad); act("All become allies", "all", "ally", json(), rad); act("Freeze all", "all", "freeze", json(), rad);
    act("Unfreeze all", "all", "unfreeze", json(), rad); ImGui::NewLine();
    act("Arm all with my guns", "all", "weapon", -1, rad); act("All giants", "all", "scale", 2.0, rad);
    act("All Ultimate Badass", "all", "rank", "ultimate", rad); act("Bring all here", "all", "bring", json(), rad); ImGui::NewLine();
    act("Allies back to enemies", "allies", "enemy", json(), rad); act("Kill my allies", "allies", "kill", json(), rad); act("Revive dead enemies", "all", "revive", json(), rad);
    ImGui::NewLine(); ImGui::PopID();
    ImGui::TextDisabled("%s", en_busy ? "working..." : en_msg.c_str());
    ImGui::Separator();

    // ---- world spawn swap
    ImGui::TextColored(gold, "WORLD SPAWN SWAP");
    ImGui::TextDisabled("Enemies are replaced the moment they appear in the world (more than 40 m away), so areas are already like this when you arrive.");
    static int swap_mode = 0; static char swap_v[96] = "";
    static const char* kModes[] = { "Off (the game's own enemies)", "Badass world (each faction's badasses)", "Boss world (replay / rift bosses)",
                                    "One faction everywhere", "Chaos (any enemy, anywhere)", "One character everywhere",
                                    "Random turf (made-up map, new every time)", "WORLD WAR (every faction everywhere, they fight)",
                                    "Faction shuffle (the game's real borders, new owners)", "CONQUEST (factions raid and take each other's turf)",
                                    "FULL ROSTER (each area's real spawn list, every tier unlocked)" };
    static const char* kModeIds[] = { "off", "badass", "bosses", "faction", "chaos", "def", "territory", "war", "shuffle", "conquest", "roster" };
    static bool swap_dirty = false;
    if (!swap_dirty) { const std::string cur = en_swap.value("mode", std::string("off")); for (int i = 0; i < 11; ++i) if (cur == kModeIds[i]) swap_mode = i; }   // follows the trainer
    ImGui::SetNextItemWidth(S * 420); if (ImGui::Combo("##swapmode", &swap_mode, kModes, 11)) swap_dirty = true;
    if (swap_mode == 3) { ImGui::SameLine(); ImGui::SetNextItemWidth(S * 220); ImGui::InputTextWithHint("##swapv", "Order, Bandit, Creep, Tediore ...", swap_v, sizeof swap_v); }
    if (swap_mode == 6) { ImGui::SameLine(); static int who = 0; ImGui::SetNextItemWidth(S * 260);
        if (ImGui::Combo("##terr", &who, "People factions only\0Every faction (creatures too)\0")) {} strcpy_s(swap_v, who ? "all" : "people"); }
    if (swap_mode == 5) { ImGui::SameLine(); ImGui::SetNextItemWidth(S * 260); ImGui::InputTextWithHint("##swapv", "character (part), e.g. PsychoBadass", swap_v, sizeof swap_v); }
    ImGui::SameLine(); if (ImGui::Button("Apply##swap")) { swap_dirty = false; en_swap["mode"] = kModeIds[swap_mode]; { std::lock_guard<std::mutex> lk2(g_en_mtx); g_en_swap["mode"] = kModeIds[swap_mode]; } en_call("BL.SPAWN_SWAP", { {"mode", kModeIds[swap_mode]}, {"v", std::string(swap_v)} }, false); }
    ImGui::TextDisabled("Now: %s %s  -  %d replaced. Never touches bosses or mission / plot / named enemies.",
        en_swap.value("mode", std::string("off")).c_str(), en_swap.value("v", std::string()).c_str(), en_swap.value("replaced", 0));
    if (en_swap.contains("territory")) { const json& t = en_swap["territory"];
        ImGui::TextColored(ImVec4(.45f, .9f, .45f, 1), "Your turf now: %s   |  north %s, south %s, east %s, west %s  (squares of 400 m)", t.value("here", std::string()).c_str(),
            t.value("north", std::string()).c_str(), t.value("south", std::string()).c_str(), t.value("east", std::string()).c_str(), t.value("west", std::string()).c_str()); }
    // ---- real territories (learned as you play)
    if (en_swap.contains("turf") && en_swap["turf"].is_object()) {
        const json& t = en_swap["turf"];
        ImGui::TextColored(gold, "TERRITORIES");
        ImGui::SameLine(); ImGui::TextDisabled("learned from where the game spawns each faction - %d squares of 200 m mapped so far (it grows as you explore)", t.value("cells_known", 0));
        ImGui::Text("This square: natively %s,  now held by  ", t.value("native", std::string("?")).c_str());
        ImGui::SameLine(0, 0); ImGui::TextColored(ImVec4(1, .55f, .3f, 1), "%s", t.value("owner", std::string("?")).c_str());
        if (t.value("conquered_cells", 0) > 0) { ImGui::SameLine(); ImGui::TextDisabled("(%d squares conquered)", t.value("conquered_cells", 0)); }
        if (en_swap.value("mode", std::string()) == "shuffle" && en_swap.contains("shuffle")) {
            std::string m; for (auto& kv : en_swap["shuffle"].items()) { if (!m.empty()) m += ",  "; m += kv.key() + " -> " + kv.value().get<std::string>(); }
            ImGui::TextWrapped("Shuffle: %s", m.c_str());
            if (ImGui::Button("Reshuffle")) en_call("BL.SPAWN_SWAP", { {"mode", "shuffle"}, {"reshuffle", true} }, false);
        }
        if (en_swap.value("mode", std::string()) == "conquest") {
            static float rm = -1; if (rm < 0) rm = (float)en_swap.value("raid_minutes", 3.0);
            ImGui::SetNextItemWidth(S * 240);
            if (ImGui::SliderFloat("raid every##rm", &rm, 0.5f, 20.f, "%.1f min")) en_call("BL.SPAWN_SWAP", { {"raid_minutes", rm} }, false);
            ImGui::SameLine(); if (ImGui::Button("Start a raid now")) en_call("BL.SPAWN_SWAP", { {"raid_now", true} }, false);
            ImGui::SameLine(); if (ImGui::Button("Give every square back to its natives")) en_call("BL.SPAWN_SWAP", { {"reset_conquest", true} }, false);
            ImGui::SameLine(); ImGui::TextDisabled("%d raid(s) going on", en_swap.value("raids", 0));
            const json lg = en_swap.value("log", json::array());
            for (int i = (int)lg.size() - 1, shown = 0; i >= 0 && shown < 6; --i, ++shown) ImGui::BulletText("%s", lg[i].value("text", std::string()).c_str());
            ImGui::TextDisabled("Raids pick a camp 60-200 m from you. Wipe out the defenders = the square changes hands for good (saved). Turn on Invisible (F4) to watch.");
        }
    }
    ImGui::Separator();

    // ---- nearby list
    ImGui::TextColored(gold, "NEARBY (150 m)"); ImGui::SameLine(); ImGui::TextDisabled("%d", (int)en_list.size());
    if (ImGui::BeginTable("##enl", 7, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, S * 260))) {
        ImGui::TableSetupColumn("Enemy", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Faction"); ImGui::TableSetupColumn("Rank");
        ImGui::TableSetupColumn("Lvl"); ImGui::TableSetupColumn("Health"); ImGui::TableSetupColumn("m"); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (auto& e : en_list) {
            const std::string addr = e.value("addr", std::string()); ImGui::PushID(addr.c_str()); ImGui::TableNextRow();
            const bool ally = e.value("ally", false), boss = e.value("boss", false);
            ImGui::TableNextColumn();
            ImGui::TextColored(ally ? ImVec4(.45f, .9f, .45f, 1) : boss ? ImVec4(1, .5f, .3f, 1) : ImVec4(1, 1, 1, 1), "%s%s", ally ? "[ALLY] " : "", pretty_char(e.value("name", std::string())).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.value("faction", std::string()).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.value("rank", std::string()).c_str());
            ImGui::TableNextColumn(); ImGui::Text("%d", e.value("level", 0));
            ImGui::TableNextColumn(); ImGui::Text("%.0f%% of %.0f", e.value("pct", 0.0), e.value("max_health", 0.0));
            ImGui::TableNextColumn(); ImGui::Text("%.0f", e.value("dist_m", 0.0));
            ImGui::TableNextColumn();
            auto b = [&](const char* l, const char* a, json v) { if (ImGui::SmallButton(l)) { json q = { {"target", addr}, {"action", a} }; if (!v.is_null()) q["v"] = v; en_call("BL.ENEMY", q); } ImGui::SameLine(); };
            b("Kill", "kill", json()); b(ally ? "Enemy" : "Ally", ally ? "enemy" : "ally", json()); b("Go", "goto", json()); b("Bring", "bring", json()); b("Arm", "weapon", -1);
            if (ImGui::SmallButton("More")) ImGui::OpenPopup("more");
            if (ImGui::BeginPopup("more")) {
                for (int i = 0; i < 7; ++i) if (ImGui::MenuItem((std::string("Rank: ") + kRankNames[i]).c_str())) en_call("BL.ENEMY", { {"target", addr}, {"action", "rank"}, {"v", kRanks[i]} });
                ImGui::Separator();
                for (double sc : { 0.4, 1.0, 1.6, 2.5, 4.0 }) { char t[32]; sprintf_s(t, "Size x%.1f", sc); if (ImGui::MenuItem(t)) en_call("BL.ENEMY", { {"target", addr}, {"action", "scale"}, {"v", sc} }); }
                ImGui::Separator();
                for (double hx : { 0.25, 2.0, 5.0, 20.0 }) { char t[32]; sprintf_s(t, "Health x%g", hx); if (ImGui::MenuItem(t)) en_call("BL.ENEMY", { {"target", addr}, {"action", "health"}, {"v", hx} }); }
                for (double dx : { 0.1, 3.0 }) { char t[32]; sprintf_s(t, "Damage x%g", dx); if (ImGui::MenuItem(t)) en_call("BL.ENEMY", { {"target", addr}, {"action", "damage"}, {"v", dx} }); }
                ImGui::Separator();
                if (ImGui::MenuItem("Freeze")) en_call("BL.ENEMY", { {"target", addr}, {"action", "freeze"} });
                if (ImGui::MenuItem("Unfreeze")) en_call("BL.ENEMY", { {"target", addr}, {"action", "unfreeze"} });
                if (ImGui::MenuItem("Drop its loot now")) en_call("BL.ENEMY", { {"target", addr}, {"action", "loot"} });
                if (ImGui::MenuItem("Yeet")) en_call("BL.ENEMY", { {"target", addr}, {"action", "launch"}, {"v", 3000} });
                ImGui::EndPopup();
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
    ImGui::Separator();

    // ---- spawn from the catalog
    ImGui::TextColored(gold, "SPAWN ANYTHING"); ImGui::SameLine(); ImGui::TextDisabled("%d characters in the game", (int)en_cat->size());
    static char filt[64] = ""; static int kind = 0, count = 1, rank = 0, arm = 0; static bool as_ally = false; static float scale = 1.f, hpx = 1.f;
    static const char* kKinds[] = { "Enemies", "Bosses", "Mini bosses", "Rift bosses", "Badass", "Loot creatures", "Turrets", "Everything (incl. NPC / parts)" };
    static const char* kKindIds[] = { "regular", "boss", "mini boss", "rift boss", "badass", "loot creature", "turret", "" };
    ImGui::SetNextItemWidth(S * 220); ImGui::InputTextWithHint("##filt", "search (psycho, earl, order...)", filt, sizeof filt);
    ImGui::SameLine(); ImGui::SetNextItemWidth(S * 200); ImGui::Combo("##kind", &kind, kKinds, 8);
    ImGui::SameLine(); ImGui::SetNextItemWidth(S * 110); ImGui::SliderInt("##cnt", &count, 1, 10, "x%d");
    ImGui::SameLine(); ImGui::Checkbox("as allies", &as_ally);
    ImGui::SameLine(); if (ImGui::Button("Random boss!")) en_call("BL.ENEMY_SPAWN", { {"name", "random_boss"}, {"ally", as_ally} });
    ImGui::SetNextItemWidth(S * 200); ImGui::Combo("rank##sr", &rank, "Their own\0Elite\0Badass\0Super Badass\0Ultimate Badass\0Mini boss\0Boss\0");
    ImGui::SameLine(); ImGui::SetNextItemWidth(S * 160); ImGui::SliderFloat("size##ss", &scale, 0.3f, 4.f, "x%.1f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(S * 160); ImGui::SliderFloat("health##sh", &hpx, 0.1f, 20.f, "x%.1f");
    ImGui::SameLine(); ImGui::SetNextItemWidth(S * 260); ImGui::Combo("weapon##sw", &arm, "Their own\0Random gun from my backpack\0");
    auto spawn = [&](const std::string& name) {
        json a = { {"name", name}, {"count", count}, {"ally", as_ally} };
        static const char* kR[] = { "", "elite", "badass", "super", "ultimate", "mini", "boss" }; if (rank) a["rank"] = kR[rank];
        if (std::fabs(scale - 1) > .01f) a["scale"] = scale; if (std::fabs(hpx - 1) > .01f) a["health"] = hpx; if (arm) a["weapon"] = -1;
        en_call("BL.ENEMY_SPAWN", a);
    };
    std::string f = filt; for (auto& c : f) c = (char)tolower((unsigned char)c);
    if (ImGui::BeginTable("##cat", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, S * 300))) {
        ImGui::TableSetupColumn("Character", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Faction"); ImGui::TableSetupColumn("Kind"); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow(); int shown = 0;
        for (auto& e : *en_cat) {
            const std::string k = e.value("kind", std::string()), n = e.value("name", std::string());
            if (kKindIds[kind][0] && k != kKindIds[kind]) continue;
            if (kind == 0 && !e.value("enemy_table", false)) continue;
            if (!f.empty()) { std::string l = n; for (auto& c : l) c = (char)tolower((unsigned char)c); if (l.find(f) == std::string::npos) continue; }
            if (++shown > 400) break;
            ImGui::PushID(n.c_str()); ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(pretty_char(n).c_str());
            if (ImGui::IsItemHovered()) { const std::string var = e.value("variant", std::string()); ImGui::SetTooltip("%s%s%s%s", n.c_str(), var.empty() ? "" : "  (", var.c_str(), var.empty() ? "" : ")"); }
            ImGui::TableNextColumn(); ImGui::TextUnformatted(e.value("faction", std::string()).c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(k.c_str());
            ImGui::TableNextColumn(); if (ImGui::SmallButton("Spawn")) spawn(n);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ PLACES page: every travel / respawn station by category
static std::mutex g_pl_mtx; static std::shared_ptr<const json> g_places = std::make_shared<const json>(json::object());
static std::string g_pl_msg; static bool g_pl_busy = false;
static void pl_go(const std::string& name, const std::string& label) {
    { std::lock_guard<std::mutex> lk(g_pl_mtx); if (g_pl_busy) return; g_pl_busy = true; g_pl_msg = "travelling to " + label + "..."; }
    bg_run([name, label] { json r = dk::cmd("BL.TRAVEL", { {"name", name} });
        std::lock_guard<std::mutex> lk(g_pl_mtx); g_pl_msg = r.value("ok", false) ? "On the way to " + label : r.value("message", std::string("failed")); g_pl_busy = false; },
          [] { std::lock_guard<std::mutex> lk(g_pl_mtx); g_pl_busy = false; g_pl_msg = "failed"; });
}
void draw_places_page(float S) {
    static bool first = true;
    if (first) { first = false; bg_run([] { json r = dk::cmd("BL.PLACES", json::object());
        std::lock_guard<std::mutex> lk(g_pl_mtx); if (r.value("ok", false)) g_places = std::make_shared<const json>(r["data"]); else g_pl_msg = r.value("message", std::string("no places")); }, [] {}); }
    std::shared_ptr<const json> P; std::string msg; bool busy;
    { std::lock_guard<std::mutex> lk(g_pl_mtx); P = g_places; msg = g_pl_msg; busy = g_pl_busy; }
    const json cats = P->value("categories", json::array()); const json places = P->value("places", json::array());
    const ImVec4 gold(1, .82f, .2f, 1);
    ImGui::TextColored(gold, "TELEPORT ANYWHERE"); ImGui::SameLine();
    ImGui::TextDisabled("%d places from the game's own travel & respawn stations - the game's fast travel takes you there", (int)places.size());
    static int cat = 0; static char filt[64] = "";
    std::vector<std::string> cn = { "All" }; for (auto& c : cats) cn.push_back(c.get<std::string>());
    std::string items; for (auto& c : cn) { items += c; items.push_back('\0'); } items.push_back('\0');
    ImGui::SetNextItemWidth(S * 300); ImGui::Combo("##plcat", &cat, items.c_str());
    ImGui::SameLine(); ImGui::SetNextItemWidth(S * 260); ImGui::InputTextWithHint("##plf", "search (boss, silo, bar, drill...)", filt, sizeof filt);
    ImGui::SameLine();
    if (ImGui::Button("Random boss room")) {
        std::vector<const json*> b; for (auto& p : places) if (p.value("category", std::string()) == "Boss rooms & arenas") b.push_back(&p);
        if (!b.empty()) { const json& p = *b[(size_t)(GetTickCount64() / 7) % b.size()]; pl_go(p.value("name", std::string()), p.value("label", std::string())); }
    }
    ImGui::TextDisabled("%s", busy ? "travelling..." : msg.c_str());
    ImGui::TextDisabled("Mission arenas may start that mission's scene - use them when you want a replay. Some spots unlock their area for the map as you arrive.");
    std::string f = filt; for (auto& c : f) c = (char)tolower((unsigned char)c);
    const std::string want = cat > 0 && cat < (int)cn.size() ? cn[cat] : "";
    if (ImGui::BeginTable("##pl", 4, ImGuiTableFlags_RowBg | ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY, ImVec2(0, S * 640))) {
        ImGui::TableSetupColumn("Place", ImGuiTableColumnFlags_WidthStretch); ImGui::TableSetupColumn("Region"); ImGui::TableSetupColumn("Category"); ImGui::TableSetupColumn("");
        ImGui::TableHeadersRow();
        for (auto& p : places) {
            const std::string c = p.value("category", std::string()), l = p.value("label", std::string()), n = p.value("name", std::string());
            if (!want.empty() && c != want) continue;
            if (!f.empty()) { std::string hay = l + " " + n + " " + c; for (auto& ch : hay) ch = (char)tolower((unsigned char)ch); if (hay.find(f) == std::string::npos) continue; }
            ImGui::PushID(n.c_str()); ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::TextUnformatted(l.c_str()); if (ImGui::IsItemHovered()) ImGui::SetTooltip("%s", n.c_str());
            ImGui::TableNextColumn(); ImGui::TextUnformatted(p.value("region", std::string()).c_str());
            ImGui::TableNextColumn(); ImGui::TextDisabled("%s", c.c_str());
            ImGui::TableNextColumn(); if (ImGui::SmallButton("Go")) pl_go(n, l);
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

// ------------------------------------------------------------------ GEAR MAKER page: any part from any manufacturer
// Start from an item you own, swap each part for the same slot from ANY list (other makers, shared element / firmware /
// grenade / shield lists) or add parts. Parts from another list are written {list:index} - the game keeps them
// (verified 2026-11-11: a Daedalus barrel on a Vladof AR, the Vladof Scrap Cannon underbarrel on a Jakobs AR).
// The new item reaches the backpack through the save queue on the next game start (BL.SAVE_RESTART).
namespace gm {
// raw = a string token between the parts (a part written by name, e.g. a legendary's unique barrel): kept exactly where
// it was, shown but not editable.
struct GP { int list = -1, index = -1; bool raw = false; Tok tok; };
static std::vector<Tok> head; static Tok comp; static bool has_comp = false;
static std::vector<Tok> tail;                  // everything from the first separator after the parts ("| 'c' , 83"): kept as it is
static std::vector<GP> parts; static int own = -1, level = 1, level0 = 1; static bool level_ok = false, have = false;
static std::string orig, src, msg; static bool msg_ok = true;
static int sel_row = -1, swap_i = -1, swap_list = -1, add_list = -1; static std::string add_slot;
static ULONGLONG restart_arm = 0;
static int new_lvl = 5; static bool new_lvl_set = false;
// perk / value editor: the part whose perks are open, what BL.PART_INFO said, typed numbers, the saved edits
static int info_i = -1; static json info; static bool info_busy = false; static std::string info_msg, info_addr;
static std::map<std::string, double> ed; static json values = json::array(); static bool values_busy = false;
static std::map<std::string, std::string> tip; static std::set<std::string> tip_busy;   // part addr -> perk text (hover)   // "build a new gun" level: your character's level once read
}
static std::string gm_slot(const Part* p) { return !p ? std::string("?") : !p->gslot.empty() ? p->gslot : p->slot; }
static std::string gm_list_name(int li) {
    const Type* t = type_of(li); if (!t) return "list " + std::to_string(li);
    static const std::map<int, std::string> shared = { {1, "Elements"}, {244, "Firmware"}, {245, "Grenade parts"}, {246, "Shield parts"},
        {247, "Enhancement stats"}, {243, "Repair kit / class parts"}, {234, "Shared parts"} };
    auto s = shared.find(li); if (s != shared.end()) return s->second + "  (list " + std::to_string(li) + ")";
    return pretty(t, li) + "  (list " + std::to_string(li) + ")";
}
static std::string gm_part_label(const gm::GP& g) {
    const Type* t = type_of(g.list); const Part* p = part_of(t, g.index);
    std::string l = part_label(p, g.index);
    if (g.list != gm::own) l = "[" + (t ? (t->maker.empty() ? gm_list_name(g.list) : pretty(t, g.list)) : std::to_string(g.list)) + "] " + l;
    return l;
}
// known side effects (tested in game)
static std::string gm_warning(const gm::GP& g) {
    if (g.list == gm::own) return "";
    const Type* src = type_of(g.list); const Type* dst = type_of(gm::own); const Part* p = part_of(src, g.index);
    const std::string sl = gm_slot(p);
    if (sl.rfind("underbarrel", 0) == 0 && src && dst && src->maker != dst->maker && sl == "underbarrel")
        return "alt fire works, but the left hand floats (this gun has no " + (src ? src->maker : std::string("")) + " underbarrel grip)";
    if (src && dst && !src->maker.empty() && src->maker != dst->maker && (sl == "barrel" || sl == "scope" || sl == "grip" || sl == "magazine" || sl == "foregrip"))
        return "other maker's " + sl + ": changes the look and stats (the game renames the gun)";
    return "";
}
static const Part* gm_default_part(const Type* t, const std::string& sl) {   // the plain part of a slot ("barrel_01", "mag_01" ...)
    static const std::map<std::string, std::vector<std::string>> want = { {"Body", {"body"}}, {"barrel", {"barrel_01"}}, {"magazine", {"mag_01"}},
        {"scope", {"scope_01_lens_01", "scope_ironsight"}}, {"grip", {"grip_01"}}, {"foregrip", {"foregrip_01"}} };
    const Part* any = nullptr;
    for (auto& q : t->parts) { if (gm_slot(&q) != sl) continue; if (!any || q.index < any->index) any = &q;
        auto w = want.find(sl); if (w == want.end()) continue;
        const size_t d = q.name.find(".part_"); const std::string n = d == std::string::npos ? q.name : q.name.substr(d + 6);
        for (auto& x : w->second) if (n == x) return &q; }
    return any;
}
static void gm_new(int list, int rarity, int level) {
    const Type* t = type_of(list); if (!t) return;
    auto val = [](uint64_t v) { Tok k; k.kind = Tok::Value; k.val.kind = serial::Val::Int; k.val.v = v; return k; };
    auto soft = []() { Tok k; k.kind = Tok::Soft; return k; };
    auto sep = []() { Tok k; k.kind = Tok::Sep; return k; };
    gm::head = { val((uint64_t)list), soft(), val(0), soft(), val(1), soft(), val((uint64_t)level), sep(), val(2), soft(), val(1 + GetTickCount64() % 4000), sep(), sep() };
    gm::comp = Tok(); gm::comp.kind = Tok::Part; gm::comp.sub = Tok::Plain; gm::comp.idx.v = (uint64_t)(95 + std::clamp(rarity, 0, 3)); gm::has_comp = true;
    gm::own = list; gm::level = gm::level0 = level; gm::level_ok = true; gm::parts.clear(); gm::tail.clear(); gm::src.clear();
    for (const char* sl : { "Body", "body_bolt", "barrel", "magazine", "scope", "grip", "foregrip" })   // no body_mag: on Daedalus it switches the ammo type
        if (const Part* q = gm_default_part(t, sl)) gm::parts.push_back({ list, q->index });
    gm::have = true; gm::orig.clear(); gm::swap_i = -1; gm::sel_row = -1; gm::add_list = list; gm::add_slot.clear();
    gm::msg = "New " + pretty(t, list) + " - change parts below, then Add to backpack."; gm::msg_ok = true;
}
// InventoryCompDef's PartTypeSelectionRules = the unique barrel / parts it forces). {types: {"<type>": [{comp, name,
// rarity, parts[{list, index, slot}]}]}}. Covers the legendaries that have no loot pool (boss drops): Kaos, HardDark ...
static json g_leg = json::object(); static long long g_leg_mtime = -2;
static const json& leg_recipes() {   // called with g_mtx held (panel thread)
    const std::string path = g_cfg.root + "\\legendary_recipes.json";
    const long long t = db_file_time(path);
    if (t != g_leg_mtime) { g_leg_mtime = t; g_leg = json::object();
        try { std::ifstream f(path); if (f) { json j = json::parse(f, nullptr, false); if (j.is_object() && j.contains("types") && j["types"].is_object()) g_leg = j["types"]; } }
        catch (...) { g_leg = json::object(); } }
    return g_leg;
}
static void gm_new_legendary(int list, const json& rec, int level) {
    gm_new(list, 0, level);   // header + the plain parts of every slot
    if (!gm::have) return;
    gm::comp.idx.v = (uint64_t)std::max(0, rec.value("comp", 95));   // the legendary's own comp
    for (auto& p : rec.value("parts", json::array())) {   // its forced parts replace the plain part of the same slot
        const int pl = p.value("list", -1), pi = p.value("index", -1); const std::string sl = p.value("slot", std::string());
        if (pl < 0 || pi < 0 || !part_of(type_of(pl), pi)) continue;
        bool done = false;
        for (auto& g : gm::parts) { const Part* q = part_of(type_of(g.list), g.index); if (q && gm_slot(q) == sl) { g = { pl, pi }; done = true; break; } }
        if (!done) gm::parts.push_back({ pl, pi });
    }
    gm::msg = "New legendary " + rec.value("name", std::string("?")) + " - change parts below if you like, then Add to backpack."; gm::msg_ok = true;
}
static bool gm_build(std::string& serial_out, std::string& text_out);
static json legendary_cmd(const json& a) {
    std::lock_guard<std::mutex> lk(g_mtx);
    if (!load_db()) return fail("NO_DB", g_db_msg);
    const json& L = leg_recipes(); if (L.empty()) return fail("NO_RECIPES", "legendary_recipes.json is missing from the trainer folder");
    const int ty = a.value("type", -1); auto it = L.find(std::to_string(ty)); if (it == L.end()) return fail("TYPE", "no legendaries for type " + std::to_string(ty));
    const json* rec = nullptr; const std::string want = a.value("name", std::string()); const int wc = a.value("comp", -1);
    for (auto& r : *it) { std::string n = r.value("name", std::string()), d = r.value("display", std::string()); std::string lw = want;   // Gearbox or in-game name
        std::transform(n.begin(), n.end(), n.begin(), ::tolower); std::transform(d.begin(), d.end(), d.begin(), ::tolower); std::transform(lw.begin(), lw.end(), lw.begin(), ::tolower);
        if ((wc >= 0 && r.value("comp", -1) == wc) || (!lw.empty() && (n == lw || d == lw))) { rec = &r; break; } }
    if (!rec) return fail("NOT_FOUND", "no such legendary for type " + std::to_string(ty));
    gm_new_legendary(ty, *rec, std::clamp(a.value("level", 50), 1, 60));
    std::string ser, txt; if (!gm_build(ser, txt)) return fail("ENCODE", "the legendary does not encode");
    return ok({ {"serial", ser}, {"text", txt}, {"name", rec->value("name", std::string())} });
}
static void gm_open(const std::string& s0, bool from_bag) {
    const std::string s = s0;   // (s0 may be gm::src itself)
    Item it; std::string err;
    if (!serial::decode(s, it, &err)) { gm::have = false; gm::msg = "Not a serial: " + err; gm::msg_ok = false; return; }
    gm::head.clear(); gm::parts.clear(); gm::tail.clear(); gm::has_comp = false; gm::own = serial::type_index(it);
    // the level exists only in the usual header; other layouts have no level and are never written to
    gm::level_ok = serial::level_tok(it) >= 0; gm::level = gm::level0 = gm::level_ok ? serial::level(it) : 0;
    const int cp = serial::comp_part(it);   // -1 when the comp is written by name: it then stays in the header as it is
    bool seen_part = false, in_tail = false;
    for (size_t i = 0; i < it.toks.size(); ++i) {
        const Tok& k = it.toks[i];
        if (in_tail) { gm::tail.push_back(k); continue; }
        if (k.kind != Tok::Part) {
            if (!seen_part) gm::head.push_back(k);
            else if (k.kind == Tok::Str) { gm::GP g; g.raw = true; g.tok = k; gm::parts.push_back(g); }   // a part written by name
            else { in_tail = true; gm::tail.push_back(k); }   // a separator after the parts: the rest is not parts of this item
            continue;
        }
        seen_part = true;
        if ((int)i == cp) { gm::comp = k; gm::has_comp = true; continue; }
        if (k.sub == Tok::Plain) gm::parts.push_back({ gm::own, (int)k.idx.v });
        else if (k.sub == Tok::One) gm::parts.push_back({ (int)k.idx.v, (int)k.one.v });
        else for (auto& v : k.list) if (v.kind != serial::Val::Soft) gm::parts.push_back({ (int)k.idx.v, (int)v.v });
    }
    gm::have = true; gm::orig = from_bag ? s : std::string(); gm::src = s; gm::msg.clear(); gm::swap_i = -1;
}
static bool gm_build(std::string& serial_out, std::string& text_out) {
    Item it; it.toks = gm::head; it.nbytes = 0;
    if (gm::has_comp) it.toks.push_back(gm::comp);
    for (auto& g : gm::parts) {
        if (g.raw) { it.toks.push_back(g.tok); continue; }   // kept as it was
        Tok t; t.kind = Tok::Part;
        if (g.list == gm::own) { t.sub = Tok::Plain; t.idx.v = (uint64_t)g.index; }
        else { t.sub = Tok::One; t.idx.v = (uint64_t)g.list; t.one.kind = serial::Val::Int; t.one.v = (uint64_t)g.index; }
        it.toks.push_back(t);
    }
    it.toks.insert(it.toks.end(), gm::tail.begin(), gm::tail.end());
    // the level is written only when this item has a level field and the user changed it
    if (gm::level_ok && gm::level != gm::level0 && !serial::set_level(it, gm::level)) return false;
    serial_out = serial::encode(it);
    Item back; if (!serial::decode(serial_out, back) || serial::encode(back) != serial_out) return false;
    text_out = serial::text(back); return true;
}
// every list that has parts of game slot `sl` (for "swap")
static std::vector<int> gm_lists_with_slot(const std::string& sl) {
    std::vector<int> out; for (auto& [i, t] : types_now()) for (auto& p : t.parts) if (gm_slot(&p) == sl) { out.push_back(i); break; }
    return out;
}
static void gm_fetch_info(const std::string& addr) {   // g_mtx held by the caller
    gm::info_busy = true; gm::info_msg = "reading..."; gm::info_addr = addr;
    bg_run([addr] { json r = dk::cmd("BL.PART_INFO", { {"addr", addr} });
                    std::lock_guard<std::mutex> lk(g_mtx); gm::info_busy = false;
                    if (r.value("ok", false)) { gm::info = r["data"]; gm::info_msg.clear(); gm::ed.clear(); } else { gm::info = json(); gm::info_msg = r.value("message", std::string("failed")); } },
           [] { std::lock_guard<std::mutex> lk(g_mtx); gm::info_busy = false; gm::info_msg = "could not read the part"; });
}
// The card's display rules (NumericDisplayValue), applied in the game's order: reduction math (a negative scale x is a
// slowdown 1/(1-x): -1 -> half -> "50%"), one plus (1+x) / one minus (1-x) / inverse (1/x), then percent (x100).
// shown_of() = game value -> what the card shows; game_of() = the exact inverse (edits are typed in shown units).
struct Disp { bool percent = false, one_plus = false, one_minus = false, inverse = false, reduction = false, plus = false; int prec = 0; };
static Disp disp_of(const json& v) {
    Disp d; if (!v.is_object()) return d;
    d.percent = v.value("percent", false); d.one_plus = v.value("one_plus", false); d.one_minus = v.value("one_minus", false);
    d.inverse = v.value("inverse", false); d.reduction = v.value("reduction", false); d.plus = v.value("plus_sign", false);
    d.prec = (int)v.value("precision", 0.0); return d;
}
static double shown_of(double x, const Disp& d) {
    if (d.reduction && x < 0) x = 1.0 - 1.0 / (1.0 - x);   // -1 -> 0.5
    if (d.one_plus) x = 1.0 + x; else if (d.one_minus) x = 1.0 - x;
    if (d.inverse && x != 0) x = 1.0 / x;
    if (d.percent) x *= 100.0;
    return x;
}
static double game_of(double s, const Disp& d, double was) {   // inverse of shown_of; `was` decides the reduction branch
    if (d.percent) s /= 100.0;
    if (d.inverse && s != 0) s = 1.0 / s;
    if (d.one_plus) s = s - 1.0; else if (d.one_minus) s = 1.0 - s;
    if (d.reduction && was < 0 && s < 1.0) s = 1.0 - 1.0 / (1.0 - s);   // 0.5 -> -1
    return s;
}
static std::string shown_text(double x, const Disp& d) {
    const double v = shown_of(x, d); char b[64];
    const int dec = d.prec > 0 ? std::min(d.prec, 4) : (std::fabs(v - std::round(v)) < 0.05 ? 0 : (std::fabs(v) < 10 ? 2 : 1));
    snprintf(b, sizeof b, "%s%.*f%s", (d.plus && v > 0) ? "+" : "", dec, v, d.percent ? "%" : "");
    return b;
}
// what a part does, one line per perk with the numbers filled in (+ how many stat effects) - for hover tooltips
static std::string gm_perk_text(const json& info) {
    std::string out;
    for (auto& pk : info.value("perks", json::array())) {
        std::string text = pk.value("text", std::string()); const json args = pk.value("args", json::object());
        for (auto& [k, v] : args.items()) { const json src = v.value("source", json()); if (!src.is_object() || !src.contains("value") || !src["value"].is_number()) continue;
            const std::string num = shown_text(src["value"].get<double>(), disp_of(v));
            const std::string ph = "{" + k + "}"; for (size_t at; (at = text.find(ph)) != std::string::npos; ) text.replace(at, ph.size(), num); }
        if (!text.empty()) out += (out.empty() ? "" : "\n") + text;
    }
    const size_t ne = info.value("effects", json::array()).size();
    if (out.empty()) out = "No perk text.";
    if (ne) out += "\n(" + std::to_string(ne) + " stat effects - open Perks after picking it)";
    return out;
}
static void gm_part_tooltip(const Part* q) {   // call right after the Selectable of a part
    if (!q || q->addr.empty() || !ImGui::IsItemHovered()) return;
    auto it = gm::tip.find(q->addr);
    if (it == gm::tip.end()) {
        if (!gm::tip_busy.count(q->addr)) { const std::string addr = q->addr; gm::tip_busy.insert(addr);
            bg_run([addr] { json r = dk::cmd("BL.PART_INFO", { {"addr", addr} });
                            std::lock_guard<std::mutex> lk(g_mtx); gm::tip_busy.erase(addr);
                            gm::tip[addr] = r.value("ok", false) ? gm_perk_text(r["data"]) : std::string("(could not read this part)"); },
                   [addr] { std::lock_guard<std::mutex> lk(g_mtx); gm::tip_busy.erase(addr); }); }
        ImGui::SetTooltip("reading...");
    } else ImGui::SetTooltip("%s", it->second.c_str());
}
static void gm_fetch_values() {
    gm::values_busy = true;
    bg_run([] { json r = dk::cmd("BL.VALUES", json::object()); std::lock_guard<std::mutex> lk(g_mtx); gm::values_busy = false;
                gm::values = r.value("ok", false) ? r["data"].value("overrides", json::array()) : json::array(); },
           [] { std::lock_guard<std::mutex> lk(g_mtx); gm::values_busy = false; });
}
static void gm_set_value(const json& src, double v) {   // a table cell -> BL.VALUE_SET (kept across restarts), then read the part again
    json a = { {"kind", "table"}, {"table", src.value("table", "")}, {"row", src.value("row", "")}, {"column", src.value("column", "")}, {"value", v} };
    const std::string addr = gm::info_addr;
    bg_run([a, addr] { json r = dk::cmd("BL.VALUE_SET", a);
                       std::lock_guard<std::mutex> lk(g_mtx);
                       gm::info_msg = r.value("ok", false) ? std::string("Saved - every gun with this part uses the new value, also after a restart.") : "Not changed: " + r.value("message", std::string());
                       if (!addr.empty()) gm_fetch_info(addr); gm_fetch_values(); },
           [] { std::lock_guard<std::mutex> lk(g_mtx); gm::info_msg = "could not change the value"; });
}
// one editable number: a table cell (percent shown x100). Constants inside the part are shown, not edited.
static void gm_value_row(const std::string& label, const json& src, const Disp& dsp, float S) {
    if (!src.is_object()) { ImGui::TextDisabled("%s: (no number)", label.c_str()); return; }
    const std::string kind = src.value("kind", std::string());
    if (kind != "table" || !src.contains("value")) {
        if (kind == "constant") ImGui::TextDisabled("%s: %s (fixed in the part)", label.c_str(), shown_text(src.value("value", 0.0), dsp).c_str());
        else ImGui::TextDisabled("%s: from %s", label.c_str(), kind == "attribute" ? src.value("attribute", std::string("another stat")).c_str() : "the game");
        return; }
    const std::string key = src.value("table", "") + "|" + src.value("row", "") + "|" + src.value("column", "");
    const double raw = src.value("value", 0.0), cur = shown_of(raw, dsp);
    if (!gm::ed.count(key)) gm::ed[key] = std::round(cur * 1000.0) / 1000.0;
    ImGui::PushID(key.c_str());
    ImGui::TextUnformatted(label.c_str()); ImGui::SameLine(S * 300); ImGui::SetNextItemWidth(S * 140);
    ImGui::InputDouble("##v", &gm::ed[key], 0, 0, dsp.percent ? "%.1f %%" : "%.3f");
    ImGui::SameLine(); if (ImGui::SmallButton("Set")) gm_set_value(src, game_of(gm::ed[key], dsp, raw));
    ImGui::SameLine(); ImGui::TextDisabled("now %s (game value %g)   [%s / %s / %s]", shown_text(raw, dsp).c_str(), raw, src.value("table", "").c_str(), src.value("row", "").c_str(), src.value("column", "").c_str());
    ImGui::PopID();
}
static void gm_draw_info(float S) {
    const ImVec4 gold(1, .82f, .2f, 1);
    ImGui::Indent(S * 20);
    if (gm::info_busy) ImGui::TextDisabled("reading the part...");
    if (!gm::info_msg.empty()) ImGui::TextDisabled("%s", gm::info_msg.c_str());
    if (gm::info.is_object()) {
        const json perks = gm::info.value("perks", json::array());
        if (perks.empty()) ImGui::TextDisabled("No perk text on this part.");
        for (auto& pk : perks) {
            std::string text = pk.value("text", std::string());
            const json args = pk.value("args", json::object());   // kept: looping over a temporary's items() read freed memory
            for (auto& [k, v] : args.items()) {   // fill the placeholders with the numbers
                const json src = v.value("source", json()); if (!src.is_object() || !src.contains("value")) continue;
                if (!src["value"].is_number()) continue;
                const std::string num = shown_text(src["value"].get<double>(), disp_of(v));
                const std::string ph = "{" + k + "}"; for (size_t at; (at = text.find(ph)) != std::string::npos; ) text.replace(at, ph.size(), num); }
            ImGui::TextColored(gold, "Perk:"); ImGui::SameLine(); ImGui::TextWrapped("%s", text.c_str());
            for (auto& [k, v] : args.items()) gm_value_row("   " + k, v.value("source", json()), disp_of(v), S);
        }
        const json eff = gm::info.value("effects", json::array());
        if (!eff.empty() && ImGui::TreeNode("##eff", "Stat effects (%d)", (int)eff.size())) {
            for (auto& e : eff) gm_value_row(e.value("attribute", std::string()) + " (" + e.value("how", std::string()) + ")", e.value("source", json()), Disp(), S);
            ImGui::TreePop(); }
        ImGui::TextDisabled("Numbers belong to the part type: changing one changes every gun with this part.");
    }
    ImGui::Unindent(S * 20);
}
void draw_gearmaker_page(float S) {
    std::lock_guard<std::mutex> lk(g_mtx);
    const ImVec4 gold(1, .82f, .2f, 1), red(1, .45f, .35f, 1), green(.45f, 1, .55f, 1);
    ImGui::TextColored(gold, "GEAR MAKER"); ImGui::SameLine();
    ImGui::TextDisabled("Pick an item you own, then swap or add parts from ANY manufacturer - the look of one, the functions of another.");
    if (!load_db()) { ImGui::TextWrapped("%s - open the Items tab once to build it.", g_db_msg.c_str()); return; }
    const float w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(g_busy ? "Reading..." : "Refresh my items")) refresh_list();
    ImGui::SameLine(); ImGui::TextDisabled("%s", g_list_msg.c_str());
    if (g_rows.empty() && !g_busy && g_list_msg.empty()) refresh_list();
    if (gm::sel_row >= (int)g_rows.size() || (gm::sel_row >= 0 && g_rows[gm::sel_row].serial != gm::src)) gm::sel_row = -1;   // the list was read again
    ImGui::BeginChild("##gm_list", ImVec2(w, S * 130), ImGuiChildFlags_Borders);
    for (int i = 0; i < (int)g_rows.size(); ++i) {
        const Row& r = g_rows[i]; char lab[256]; snprintf(lab, sizeof lab, "%s %d   %s##gm%d", r.where == "ground" ? "Ground" : "Bag", r.slot, r.title.c_str(), i);
        if (ImGui::Selectable(lab, gm::sel_row == i)) { gm::sel_row = i; gm_open(r.serial, r.where == "backpack"); }
    }
    ImGui::EndChild();
    {   // ---- or a new gun from scratch
        static int s_type = 2, s_maker = 0, s_rar = 3; int& s_lvl = gm::new_lvl;
        if (!gm::new_lvl_set) { gm::new_lvl_set = true;   // start at the character's level (was always 5)
            bg_run([] { json r = dk::cmd("BL.STATE", json::object()); double lv = 0;
                        try { lv = r["data"]["player"].value("level", 0.0); } catch (...) {}
                        if (lv >= 1) { std::lock_guard<std::mutex> lk2(g_mtx); gm::new_lvl = std::clamp((int)lv, 1, 60); } },
                   [] {}); }
        static const char* tnames[] = { "Pistol", "Shotgun", "Assault Rifle", "SMG", "Sniper" };
        static const char* tkeys[] = { "Weapon_PS", "Weapon_SG", "Weapon_AR", "Weapon_SM", "Weapon_SR" };
        static const char* rnames[] = { "Common", "Uncommon", "Rare", "Epic" };
        std::vector<int> lists; for (auto& [li, t] : types_now()) if (t.name == tkeys[s_type] && !t.maker.empty() && gm_default_part(&t, "Body")) lists.push_back(li);
        if (s_maker >= (int)lists.size()) s_maker = 0;
        ImGui::TextColored(gold, "Or build a new gun:"); ImGui::SameLine();
        ImGui::SetNextItemWidth(S * 150); if (ImGui::Combo("##st", &s_type, tnames, 5)) s_maker = 0;
        ImGui::SameLine(); ImGui::SetNextItemWidth(S * 190);
        if (ImGui::BeginCombo("##sm", lists.empty() ? "-" : pretty(type_of(lists[s_maker]), lists[s_maker]).c_str())) {
            for (int i = 0; i < (int)lists.size(); ++i) if (ImGui::Selectable(pretty(type_of(lists[i]), lists[i]).c_str(), i == s_maker)) s_maker = i;
            ImGui::EndCombo(); }
        ImGui::SameLine(); ImGui::SetNextItemWidth(S * 130); ImGui::Combo("##sr", &s_rar, rnames, 4);
        ImGui::SameLine(); ImGui::SetNextItemWidth(S * 110); if (ImGui::InputInt("lvl##sl", &s_lvl)) s_lvl = std::clamp(s_lvl, 1, 60);
        ImGui::SameLine(); if (ImGui::Button("Start") && !lists.empty()) gm_new(lists[s_maker], s_rar, s_lvl);
        {   // ---- any legendary of this gun type, also the ones no loot pool drops (boss drops)
            static int s_leg = 0;
            const json& L = leg_recipes();
            struct LG { int list; const json* rec; std::string label; };
            std::vector<LG> legs;
            for (int li : lists) { auto it = L.find(std::to_string(li)); if (it == L.end() || !it->is_array()) continue;
                for (auto& r : *it) { std::string n = r.value("name", std::string("?")); if (!n.empty()) n[0] = (char)toupper((unsigned char)n[0]);
                    { const std::string dn = r.value("display", std::string()); if (!dn.empty() && _stricmp(dn.c_str(), n.c_str()) != 0) n = dn + " (" + n + ")"; }   // in-game name first
                    if (r.value("rarity", std::string()).find("pearl") != std::string::npos) n += " (pearl)";
                    legs.push_back({ li, &r, n + "  -  " + pretty(type_of(li), li) }); } }
            std::sort(legs.begin(), legs.end(), [](const LG& a, const LG& b) { return a.label < b.label; });
            if (s_leg >= (int)legs.size()) s_leg = 0;
            ImGui::TextColored(gold, "Or any legendary:"); ImGui::SameLine(); ImGui::SetNextItemWidth(S * 420);
            if (ImGui::BeginCombo("##lg", legs.empty() ? (L.empty() ? "(no legendary_recipes.json)" : "-") : legs[s_leg].label.c_str(), ImGuiComboFlags_HeightLarge)) {
                static char flt[96] = {};   // type to narrow the list (every word must appear); Enter takes the first match
                if (ImGui::IsWindowAppearing()) { flt[0] = 0; ImGui::SetKeyboardFocusHere(); }
                ImGui::SetNextItemWidth(-1);
                const bool enter = ImGui::InputTextWithHint("##lgf", "Type to search...", flt, sizeof flt, ImGuiInputTextFlags_EnterReturnsTrue);
                ImGui::Separator();
                auto match = [&](const std::string& lab) { std::string l = lab, q = flt; for (auto& c : l) c = (char)tolower((unsigned char)c); for (auto& c : q) c = (char)tolower((unsigned char)c);
                    size_t i = 0; while (i < q.size()) { size_t j = q.find(' ', i); const std::string w = q.substr(i, j == std::string::npos ? std::string::npos : j - i);
                        if (!w.empty() && l.find(w) == std::string::npos) return false; if (j == std::string::npos) break; i = j + 1; } return true; };
                int first = -1;
                for (int i = 0; i < (int)legs.size(); ++i) { if (flt[0] && !match(legs[i].label)) continue; if (first < 0) first = i;
                    if (ImGui::Selectable((legs[i].label + "##lg" + std::to_string(i)).c_str(), i == s_leg)) s_leg = i; }
                if (first < 0) ImGui::TextDisabled("No match");
                if (enter && first >= 0) { s_leg = first; ImGui::CloseCurrentPopup(); }
                ImGui::EndCombo(); }
            ImGui::SameLine(); ImGui::TextDisabled("lvl %d", s_lvl);
            ImGui::SameLine(); if (ImGui::Button("Start legendary") && !legs.empty()) gm_new_legendary(legs[s_leg].list, *legs[s_leg].rec, s_lvl);
            ImGui::TextDisabled("Includes boss-only legendaries the Builder cannot drop. Pick the gun type above.");
        }
    }
    if (!gm::have) { if (!gm::msg.empty()) ImGui::TextColored(red, "%s", gm::msg.c_str()); else ImGui::TextDisabled("Choose the item to start from (its body sets the look)."); return; }

    ImGui::Separator();
    const Type* own = type_of(gm::own);
    ImGui::TextColored(gold, "%s", pretty(own, gm::own).c_str()); ImGui::SameLine();
    if (gm::level_ok) { ImGui::SetNextItemWidth(S * 140); if (ImGui::InputInt("Level##gm", &gm::level)) gm::level = std::clamp(gm::level, 1, 60); }
    else ImGui::TextDisabled("no level on this item");
    ImGui::SameLine(); ImGui::TextDisabled("(higher than your level = you can carry it but not use it yet)");

    // ---- parts
    ImGui::BeginChild("##gm_parts", ImVec2(w, S * 260), ImGuiChildFlags_Borders);
    int remove_at = -1;
    for (int i = 0; i < (int)gm::parts.size(); ++i) {
        const gm::GP& g = gm::parts[i]; const Part* p = part_of(type_of(g.list), g.index);
        ImGui::PushID(i);
        if (g.raw) {   // a part written by name: kept exactly as it is
            ImGui::TextDisabled("%-14s", "(fixed)"); ImGui::SameLine(S * 150); ImGui::TextUnformatted(g.tok.str.c_str());
            ImGui::PopID(); continue; }
        ImGui::TextDisabled("%-14s", gm_slot(p).c_str()); ImGui::SameLine(S * 150);
        ImGui::TextUnformatted(gm_part_label(g).c_str());
        const std::string wn = gm_warning(g);
        ImGui::SameLine(w - S * 240);
        if (p && !p->addr.empty()) { if (ImGui::SmallButton(gm::info_i == i ? "hide" : "Perks")) { if (gm::info_i == i) gm::info_i = -1; else { gm::info_i = i; gm::info = json(); gm_fetch_info(p->addr); } } }
        else ImGui::TextDisabled("     ");
        ImGui::SameLine(); if (ImGui::SmallButton(gm::swap_i == i ? "close" : "Swap")) { gm::swap_i = gm::swap_i == i ? -1 : i; gm::swap_list = -1; }
        ImGui::SameLine(); if (ImGui::SmallButton("Remove")) remove_at = i;
        if (!wn.empty()) { ImGui::TextColored(red, "      ! %s", wn.c_str()); }
        if (gm::swap_i == i) {   // same slot, any list
            const std::string sl = gm_slot(p); auto lists = gm_lists_with_slot(sl);
            if (gm::swap_list < 0) gm::swap_list = g.list;
            ImGui::Indent(S * 20);
            ImGui::SetNextItemWidth(S * 360);
            if (ImGui::BeginCombo("from##sw", gm_list_name(gm::swap_list).c_str())) {
                for (int li : lists) if (ImGui::Selectable(gm_list_name(li).c_str(), li == gm::swap_list)) gm::swap_list = li;
                ImGui::EndCombo();
            }
            const Type* st = type_of(gm::swap_list);
            if (st) for (auto& q : st->parts) { if (gm_slot(&q) != sl) continue;
                char b2[256]; snprintf(b2, sizeof b2, "%s##sp%d", part_label(&q, q.index).c_str(), q.index);
                if (ImGui::Selectable(b2, gm::swap_list == g.list && q.index == g.index)) { gm::parts[i] = { gm::swap_list, q.index }; }
                gm_part_tooltip(&q); }
            ImGui::Unindent(S * 20);
        }
        if (gm::info_i == i) gm_draw_info(S);
        ImGui::PopID();
    }
    if (remove_at >= 0) { gm::parts.erase(gm::parts.begin() + remove_at); gm::swap_i = -1; gm::info_i = -1; }
    ImGui::EndChild();

    // ---- the value edits that stay (BL.VALUES)
    if (ImGui::CollapsingHeader("Edited values (kept after a restart)")) {
        static bool asked = false; if (!asked && !gm::values_busy) { asked = true; gm_fetch_values(); }
        if (ImGui::SmallButton("Refresh")) gm_fetch_values();
        if (gm::values.empty()) ImGui::TextDisabled("Nothing changed yet - open a part's Perks and set a number.");
        int reset_i = -1;
        for (int k = 0; k < (int)gm::values.size(); ++k) { const json& e = gm::values[k]; ImGui::PushID(k + 9000);
            ImGui::Text("%s / %s / %s:  %s  (was %s)", e.value("table", e.value("name", std::string())).c_str(), e.value("row", std::string()).c_str(),
                        e.value("column", e.value("field", std::string())).c_str(), e.value("value", json()).dump().c_str(), e.value("orig", json()).dump().c_str());
            ImGui::SameLine(); if (ImGui::SmallButton("Reset")) reset_i = k; ImGui::PopID(); }
        if (reset_i >= 0) { const int ri = reset_i; bg_run([ri] { dk::cmd("BL.VALUE_RESET", { {"index", ri} }); std::lock_guard<std::mutex> lk(g_mtx); gm_fetch_values(); if (!gm::info_addr.empty()) gm_fetch_info(gm::info_addr); }, [] {}); }
    }
    // ---- add a part from any list
    ImGui::TextColored(gold, "Customize"); ImGui::SameLine();
    ImGui::TextDisabled("pick a feature, then a part below - one-per-slot parts (barrel, magazine, grip, alt fire, element...) replace the old one");
    if (gm::add_list < 0) gm::add_list = gm::own;
    {
        auto has_slot = [](int li, const char* sl) { const Type* t = type_of(li); if (!t) return false; for (auto& q : t->parts) if (gm_slot(&q) == sl) return true; return false; };
        const Type* ot = type_of(gm::own); const std::string on = ot ? ot->name : "";
        const bool gun = on.rfind("Weapon_", 0) == 0;
        int vla = -1;   // Vladof list of the same weapon type (alt fire underbarrels), else any Vladof list with one
        if (gun) { for (auto& [li, t] : types_now()) if (t.name == on && t.maker == "VLA" && has_slot(li, "underbarrel")) { vla = li; break; }
                   if (vla < 0) for (auto& [li, t] : types_now()) if (t.maker == "VLA" && has_slot(li, "underbarrel")) { vla = li; break; } }
        const bool gren = on.find("grenade") != std::string::npos, shield = on.find("shield") != std::string::npos, enh = on.find("nhancement") != std::string::npos;
        struct SC { const char* label; int list; const char* slot; };
        auto row = [&](const char* title, std::initializer_list<SC> sc) {
            bool any = false; for (auto& c : sc) if (c.list >= 0 && has_slot(c.list, c.slot)) any = true;
            if (!any) return;
            ImGui::TextDisabled("%-12s", title); float x = S * 110; const float maxw = w - S * 10;
            for (auto& c : sc) {
                if (c.list < 0 || !has_slot(c.list, c.slot)) continue;
                const float bw = ImGui::CalcTextSize(c.label).x + S * 16;
                if (x + bw < maxw) ImGui::SameLine(); else { x = S * 110; ImGui::Dummy(ImVec2(S * 100, 0)); ImGui::SameLine(); }
                const bool on_now = gm::add_list == c.list && gm::add_slot == c.slot;
                if (on_now) ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(.55f, .42f, .1f, 1));
                if (ImGui::Button(c.label)) { gm::add_list = c.list; gm::add_slot = c.slot; }
                if (on_now) ImGui::PopStyleColor();
                x += bw + S * 8;
            }
        };
        if (gun) {
            row("Bullets", { { "Magazine / bullet type", gm::own, "magazine" }, { "Torgue normal / sticky", gm::own, "magazine_acc" },
                             { "Element", 1, "body_ele" }, { "Barrel", gm::own, "barrel" } });
            row("Fire modes", { { "Alt fire (underbarrel)", gm::own, "underbarrel" }, { "Alt fire (foregrip)", gm::own, "foregrip" },
                                { "Vladof alt fire", (ot && ot->maker != "VLA") ? vla : -1, "underbarrel" }, { "Alt fire add-on", gm::own, "underbarrel_acc" },
                                { "Switch ammo type", gm::own, "secondary_ammo" }, { "Switch element", 1, "secondary_ele" } });
            row("Perks", { { "Firmware perk", 244, "firmware" }, { "Barrel add-ons / licensed", gm::own, "barrel_acc" }, { "Grip (Hyperion / Tediore)", gm::own, "grip" },
                           { "Hyperion gun shield", gm::own, "hyperion_secondary_acc" }, { "Tediore throw-reload", gm::own, "magazine_ted_thrown" },
                           { "Body mods", gm::own, "body_acc" }, { "Scope", gm::own, "scope" }, { "Scope mods", gm::own, "scope_acc" } });
        }
        row("Grenade", { { "Behaviour", gren ? 245 : -1, "payload" }, { "Behaviour add-on", gren ? 245 : -1, "payload_augment" },
                         { "Element", gren ? 245 : -1, "Element" }, { "Stat boost", gren ? 245 : -1, "stat_augment" }, { "Firmware", gren ? 245 : -1, "firmware" } });
        row("Shield", { { "Element", shield ? 246 : -1, "Element" }, { "Main augment", shield ? 246 : -1, "primary_augment" },
                        { "2nd augment", shield ? 246 : -1, "secondary_augment" }, { "Firmware", shield ? 246 : -1, "firmware" } });
        row("Enhancement", { { "Stats 1", enh ? 247 : -1, "stat_group1" }, { "Stats 2", enh ? 247 : -1, "stat_group2" },
                             { "Stats 3", enh ? 247 : -1, "stat_group3" }, { "Firmware", enh ? 247 : -1, "firmware" } });
        if (!gun && !gren && !shield && !enh) row("Perks", { { "Firmware perk", 244, "firmware" } });
    }
    ImGui::TextDisabled("Any list / slot:");
    ImGui::SetNextItemWidth(S * 360);
    if (ImGui::BeginCombo("list##add", gm_list_name(gm::add_list).c_str())) {
        for (auto& [li, t] : types_now()) { if (t.parts.empty()) continue; if (ImGui::Selectable(gm_list_name(li).c_str(), li == gm::add_list)) { gm::add_list = li; gm::add_slot.clear(); } }
        ImGui::EndCombo();
    }
    const Type* at = type_of(gm::add_list);
    std::vector<std::string> slots; if (at) for (auto& q : at->parts) { const std::string s = gm_slot(&q); if (std::find(slots.begin(), slots.end(), s) == slots.end()) slots.push_back(s); }
    ImGui::SameLine(); ImGui::SetNextItemWidth(S * 260);
    if (ImGui::BeginCombo("slot##add", gm::add_slot.empty() ? "(choose a slot)" : gm::add_slot.c_str())) {
        for (auto& s : slots) if (ImGui::Selectable(s.c_str(), s == gm::add_slot)) gm::add_slot = s;
        ImGui::EndCombo();
    }
    if (at && !gm::add_slot.empty()) {
        ImGui::BeginChild("##gm_add", ImVec2(w, S * 110), ImGuiChildFlags_Borders);
        for (auto& q : at->parts) { if (gm_slot(&q) != gm::add_slot) continue;
            char b2[256]; snprintf(b2, sizeof b2, "+ %s##ad%d", part_label(&q, q.index).c_str(), q.index);
            const bool picked = ImGui::Selectable(b2); gm_part_tooltip(&q);
            if (picked) {
                static const std::set<std::string> single = { "Body", "barrel", "magazine", "scope", "grip", "foregrip", "underbarrel", "body_ele",
                    "secondary_ele", "secondary_ammo", "magazine_ted_thrown", "hyperion_secondary_acc", "payload", "Element", "body_mag", "body_bolt" };
                bool done = false;
                if (single.count(gm::add_slot))
                    for (auto& g : gm::parts) { const Part* pp = part_of(type_of(g.list), g.index);
                        if (pp && gm_slot(pp) == gm::add_slot) { g = { gm::add_list, q.index }; done = true; break; } }
                if (!done) gm::parts.push_back({ gm::add_list, q.index });
            } }
        ImGui::EndChild();
    }

    // ---- result
    ImGui::Separator();
    std::string ser, txt; const bool okb = gm_build(ser, txt);
    if (!okb) { ImGui::TextColored(red, "This combination does not encode - undo the last change."); return; }
    ImGui::TextDisabled("%s", txt.c_str());
    if (ImGui::Button("Add to backpack (after restart)")) { json r = dk::cmd("BL.SAVE_QUEUE", { {"serial", ser} }); gm::msg_ok = r.value("ok", false);
        gm::msg = gm::msg_ok ? "Queued as a new item - it arrives when the game restarts." : r.value("message", std::string("failed")); }
    if (!gm::orig.empty()) { ImGui::SameLine();
        if (ImGui::Button("Replace the original (after restart)")) { json r = dk::cmd("BL.SAVE_QUEUE", { {"op", "replace_item"}, {"old", gm::orig}, {"new", ser} });
            gm::msg_ok = r.value("ok", false); gm::msg = gm::msg_ok ? "Queued: the original becomes this one on the next start." : r.value("message", std::string("failed")); } }
    ImGui::SameLine();
    const bool armed = GetTickCount64() - gm::restart_arm < 5000;
    if (ImGui::Button(armed ? "Click again to restart the game" : "Restart now to get it")) {
        if (!armed) gm::restart_arm = GetTickCount64();
        else { gm::restart_arm = 0; gm::msg = "Asking the game to close..."; gm::msg_ok = true;
               // the real answer is shown (before: "Restarting" even with nothing queued or no game window)
               bg_run([] { dk::cmd("BL.CAMERA", { {"reset", true} }); json r = dk::cmd("BL.SAVE_RESTART", json::object());
                          std::lock_guard<std::mutex> lk2(g_mtx); gm::msg_ok = r.value("ok", false);
                          gm::msg = gm::msg_ok ? std::string("Restarting - the game saves, closes and starts again with your gear.") : "No restart: " + r.value("message", std::string("failed")); },
                     [] { std::lock_guard<std::mutex> lk2(g_mtx); gm::msg_ok = false; gm::msg = "The restart could not be started."; }); }
    }
    ImGui::SameLine(); if (ImGui::Button("Copy serial")) ImGui::SetClipboardText(ser.c_str());
    if (!gm::msg.empty()) ImGui::TextColored(gm::msg_ok ? green : red, "%s", gm::msg.c_str());
    ImGui::TextDisabled("Your save is backed up before every change (save_backups folder). Keep a free backpack slot for each new item.");
}
} // namespace wb::items
