// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// BL4 saves (Documents\My Games\Borderlands 4\Saved\SaveGames\<SteamID>\Profiles\client\<n>.sav, profile.sav) are
//   AES-256-ECB( zlib(YAML) + u32 uncompressed length + PKCS7 pad ),  key = 32-byte base (in the exe at .rdata, found by
// The game reads a character save ONCE (at start) and writes its in-memory state over it later, so edits are QUEUED
// (bin\save_pending.json) and applied by the next game start - this DLL applies them while the process is young, before
// the title screen loads the profile. "Apply now" closes the game (like clicking X) and the loader starts it again.
// Ops: add_item {serial} -> backpack;  set {key, value} for a fixed list of fields (skill points, SDU tokens, difficulty,
// UVH / Mayhem levels).  Every write backs the save up to bin\save_backups first.
// 2026-11-15 hardening: every file is written through .tmp + flush + rename; serials are checked (decode + encode) when
// queued and again when applied; a replace whose original is not in the save stays queued; edits never add a key to a
// save; the queue remembers the account + character guid and is only applied to that one; nothing throws out of here.
#include "common.h"
#include "serial.h"
#include <bcrypt.h>
#include <shlobj.h>
#include <shellapi.h>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <regex>
#include <ctime>
#include <cctype>
#include <algorithm>
#include <utility>
#pragma comment(lib, "bcrypt.lib")

namespace wb::sv {
namespace fs = std::filesystem;

static const uint8_t kBase[32] = { 0x35,0xEC,0x33,0x77,0xF3,0x5D,0xB0,0xEA,0xBE,0x6B,0x83,0x11,0x54,0x03,0xEB,0xFB,
                                   0x27,0x25,0x64,0x2E,0xD5,0x49,0x06,0x29,0x05,0x78,0xBD,0x60,0xBA,0x4A,0xA7,0x87 };

// ------------------------------------------------------------------ inflate (RFC 1951, after zlib's puff.c)
namespace inf {
struct St { const uint8_t* p; size_t n, pos = 0; uint32_t buf = 0; int cnt = 0; bool err = false; std::string out; };
static int bits(St& s, int need) {
    uint32_t v = s.buf;
    while (s.cnt < need) { if (s.pos >= s.n) { s.err = true; return 0; } v |= (uint32_t)s.p[s.pos++] << s.cnt; s.cnt += 8; }
    s.buf = v >> need; s.cnt -= need;
    return (int)(v & ((1u << need) - 1));
}
struct Huff { short count[16]; short symbol[320]; };
static int decode(St& s, const Huff& h) {
    int code = 0, first = 0, index = 0;
    for (int len = 1; len < 16; ++len) {
        code |= bits(s, 1); if (s.err) return -1;
        const int count = h.count[len];
        if (code - count < first) return h.symbol[index + (code - first)];
        index += count; first += count; first <<= 1; code <<= 1;
    }
    return -1;
}
static int build(Huff& h, const short* length, int n) {
    for (int l = 0; l < 16; ++l) h.count[l] = 0;
    for (int i = 0; i < n; ++i) h.count[length[i]]++;
    if (h.count[0] == n) return 0;
    int left = 1;
    for (int l = 1; l < 16; ++l) { left <<= 1; left -= h.count[l]; if (left < 0) return left; }
    short offs[16]; offs[1] = 0;
    for (int l = 1; l < 15; ++l) offs[l + 1] = offs[l] + h.count[l];
    for (int i = 0; i < n; ++i) if (length[i]) h.symbol[offs[length[i]]++] = (short)i;
    return left;
}
static bool codes(St& s, const Huff& lc, const Huff& dc) {
    static const short lb[29] = {3,4,5,6,7,8,9,10,11,13,15,17,19,23,27,31,35,43,51,59,67,83,99,115,131,163,195,227,258};
    static const short le[29] = {0,0,0,0,0,0,0,0,1,1,1,1,2,2,2,2,3,3,3,3,4,4,4,4,5,5,5,5,0};
    static const short db[30] = {1,2,3,4,5,7,9,13,17,25,33,49,65,97,129,193,257,385,513,769,1025,1537,2049,3073,4097,6145,8193,12289,16385,24577};
    static const short de[30] = {0,0,0,0,1,1,2,2,3,3,4,4,5,5,6,6,7,7,8,8,9,9,10,10,11,11,12,12,13,13};
    for (;;) {
        int sym = decode(s, lc);
        if (sym < 0 || s.err) return false;
        if (sym < 256) { s.out.push_back((char)sym); continue; }
        if (sym == 256) return true;
        sym -= 257; if (sym >= 29) return false;
        const int len = lb[sym] + bits(s, le[sym]);
        const int ds = decode(s, dc); if (ds < 0 || ds >= 30) return false;
        const size_t dist = (size_t)(db[ds] + bits(s, de[ds]));
        if (s.err || dist > s.out.size()) return false;
        for (int i = 0; i < len; ++i) s.out.push_back(s.out[s.out.size() - dist]);
    }
}
static bool run(const uint8_t* p, size_t n, std::string& out) {
    St s; s.p = p; s.n = n;
    int last;
    do {
        last = bits(s, 1); const int type = bits(s, 2); if (s.err) return false;
        if (type == 0) {
            s.buf = 0; s.cnt = 0;
            if (s.pos + 4 > s.n) return false;
            const unsigned len = s.p[s.pos] | (s.p[s.pos + 1] << 8), nlen = s.p[s.pos + 2] | (s.p[s.pos + 3] << 8); s.pos += 4;
            if (len != (~nlen & 0xFFFF) || s.pos + len > s.n) return false;
            s.out.append((const char*)s.p + s.pos, len); s.pos += len;
        } else if (type == 1) {
            struct Fixed { Huff lc, dc; Fixed() { short l[288]; int i = 0; for (; i < 144; ++i) l[i] = 8; for (; i < 256; ++i) l[i] = 9; for (; i < 280; ++i) l[i] = 7; for (; i < 288; ++i) l[i] = 8;
                                                  build(lc, l, 288); for (i = 0; i < 30; ++i) l[i] = 5; build(dc, l, 30); } };
            static const Fixed fx;
            if (!codes(s, fx.lc, fx.dc)) return false;
        } else if (type == 2) {
            static const short order[19] = {16,17,18,0,8,7,9,6,10,5,11,4,12,3,13,2,14,1,15};
            const int nlen = bits(s, 5) + 257, ndist = bits(s, 5) + 1, ncode = bits(s, 4) + 4;
            if (s.err || nlen > 286 || ndist > 30) return false;
            short l[320] = {};
            for (int i = 0; i < ncode; ++i) l[order[i]] = (short)bits(s, 3);
            Huff lc, dc;
            if (build(lc, l, 19) != 0) return false;
            int idx = 0;
            while (idx < nlen + ndist) {
                int sym = decode(s, lc); if (sym < 0) return false;
                if (sym < 16) { l[idx++] = (short)sym; continue; }
                short v = 0; int rep;
                if (sym == 16) { if (!idx) return false; v = l[idx - 1]; rep = 3 + bits(s, 2); }
                else if (sym == 17) rep = 3 + bits(s, 3);
                else rep = 11 + bits(s, 7);
                if (idx + rep > nlen + ndist) return false;
                while (rep--) l[idx++] = v;
            }
            const int e1 = build(lc, l, nlen); if (e1 < 0 || (e1 > 0 && nlen - lc.count[0] != 1)) return false;
            const int e2 = build(dc, l + nlen, ndist); if (e2 < 0 || (e2 > 0 && ndist - dc.count[0] != 1)) return false;
            if (!codes(s, lc, dc)) return false;
        } else return false;
    } while (!last);
    out.swap(s.out);
    return true;
}
} // namespace inf

static std::string zlib_store(const std::string& d) {   // a valid zlib stream of stored (uncompressed) blocks
    std::string o; o.push_back((char)0x78); o.push_back((char)0x01);
    size_t i = 0;
    do {
        const size_t n = std::min<size_t>(65535, d.size() - i);
        o.push_back((char)(i + n >= d.size() ? 1 : 0));
        o.push_back((char)(n & 255)); o.push_back((char)(n >> 8)); o.push_back((char)(~n & 255)); o.push_back((char)((~n >> 8) & 255));
        o.append(d, i, n); i += n;
    } while (i < d.size());
    uint32_t a = 1, b = 0; for (unsigned char c : d) { a = (a + c) % 65521; b = (b + a) % 65521; }
    const uint32_t ad = (b << 16) | a;
    o.push_back((char)(ad >> 24)); o.push_back((char)(ad >> 16)); o.push_back((char)(ad >> 8)); o.push_back((char)ad);
    return o;
}

// ------------------------------------------------------------------ AES-256-ECB (BCrypt)
static bool aes(bool enc, const uint8_t key[32], std::string& data) {
    BCRYPT_ALG_HANDLE alg = nullptr; BCRYPT_KEY_HANDLE k = nullptr; bool ok = false;
    if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_AES_ALGORITHM, nullptr, 0) == 0 &&
        BCryptSetProperty(alg, BCRYPT_CHAINING_MODE, (PUCHAR)BCRYPT_CHAIN_MODE_ECB, sizeof(BCRYPT_CHAIN_MODE_ECB), 0) == 0 &&
        BCryptGenerateSymmetricKey(alg, &k, nullptr, 0, (PUCHAR)key, 32, 0) == 0) {
        ULONG got = 0;
        std::string out(data.size(), '\0');
        const NTSTATUS st = enc ? BCryptEncrypt(k, (PUCHAR)data.data(), (ULONG)data.size(), nullptr, nullptr, 0, (PUCHAR)out.data(), (ULONG)out.size(), &got, 0)
                                : BCryptDecrypt(k, (PUCHAR)data.data(), (ULONG)data.size(), nullptr, nullptr, 0, (PUCHAR)out.data(), (ULONG)out.size(), &got, 0);
        if (st == 0) { out.resize(got); data.swap(out); ok = true; }
    }
    if (k) BCryptDestroyKey(k);
    if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    return ok;
}

// ------------------------------------------------------------------ paths
// Paths stay wide end to end: path::string() throws when a folder name has a character outside the ANSI code page
// (e.g. a user name), and that used to end the game at every start while edits were queued.
static std::string utf8(const fs::path& p) {   // for messages / JSON only; never throws
    try { const auto s = p.u8string(); return std::string(s.begin(), s.end()); } catch (...) { return "?"; }
}
struct Where { fs::path dir; uint64_t steamid = 0; };
static Where where() {
    Where w; PWSTR docs = nullptr;
    try {
        if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_Documents, 0, nullptr, &docs)) && docs) {
            const fs::path base = fs::path(docs) / L"My Games" / L"Borderlands 4" / L"Saved" / L"SaveGames";
            std::error_code ec; fs::file_time_type best{};
            for (fs::directory_iterator it(base, ec), end; !ec && it != end; it.increment(ec)) {
                const fs::directory_entry& e = *it;
                std::error_code e2; if (!e.is_directory(e2) || e2) continue;
                const std::wstring n = e.path().filename().wstring();
                if (n.size() < 15 || n.find_first_not_of(L"0123456789") != std::wstring::npos) continue;
                const fs::path c = e.path() / L"Profiles" / L"client";
                const auto t = fs::last_write_time(c / L"profile.sav", e2); if (e2) continue;
                if (w.dir.empty() || t > best) { best = t; w.dir = c; w.steamid = _wcstoui64(n.c_str(), nullptr, 10); }
            }
        }
    } catch (...) { w = Where(); }
    if (docs) CoTaskMemFree(docs);
    return w;
}
static void key_for(uint64_t sid, uint8_t k[32]) { memcpy(k, kBase, 32); for (int i = 0; i < 8; ++i) k[i] ^= (uint8_t)(sid >> (8 * i)); }

static bool read_file(const fs::path& f, std::string& d) {
    std::ifstream in(f, std::ios::binary); if (!in) return false;
    d.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return !in.bad();
}
// The whole file goes to <name>.tmp, is flushed to the disk and only then renamed over the target, so the target is
// never half written (full disk, crash, power loss).
static bool write_file_atomic(const fs::path& f, const std::string& d, std::string& err) {
    fs::path tmp = f; tmp += L".tmp";
    HANDLE h = CreateFileW(tmp.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (h == INVALID_HANDLE_VALUE) { err = "cannot write " + utf8(tmp) + " (error " + std::to_string(GetLastError()) + ")"; return false; }
    bool okw = true; size_t off = 0;
    while (okw && off < d.size()) {
        DWORD put = 0; const DWORD n = (DWORD)std::min<size_t>(d.size() - off, (size_t)1 << 20);
        okw = WriteFile(h, d.data() + off, n, &put, nullptr) != 0 && put > 0; off += put;
    }
    if (okw) okw = FlushFileBuffers(h) != 0;
    const DWORD werr = okw ? 0 : GetLastError();
    CloseHandle(h);
    if (!okw) { DeleteFileW(tmp.c_str()); err = "write failed (error " + std::to_string(werr) + ") - nothing changed"; return false; }
    if (!MoveFileExW(tmp.c_str(), f.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        err = "replace failed (error " + std::to_string(GetLastError()) + ") - nothing changed"; DeleteFileW(tmp.c_str()); return false; }
    return true;
}

static bool read_yaml(const fs::path& f, uint64_t sid, std::string& yaml, std::string& err) {
    std::string d; if (!read_file(f, d)) { err = "cannot open " + utf8(f); return false; }
    if (d.empty() || d.size() % 16) { err = "not a BL4 save (size)"; return false; }
    uint8_t k[32]; key_for(sid, k);
    if (!aes(false, k, d)) { err = "decrypt failed"; return false; }
    if (d.empty()) { err = "decrypt failed"; return false; }
    if (d.size() < 10 || (unsigned char)d[0] != 0x78) { err = "wrong key (no zlib header)"; return false; }
    // The game does not always pad the same way: usually PKCS#7, but a file whose data already ends on a 16-byte
    // boundary is written unpadded or zero-filled (seen on profile.sav 2026-11-15). Try each layout; the one whose
    // inflated size matches the length trailer wins.
    std::vector<size_t> cut;
    { const unsigned pad = (unsigned char)d.back();
      if (pad >= 1 && pad <= 16 && pad < d.size()) { bool same = true; for (size_t i = d.size() - pad; i < d.size(); ++i) same = same && (unsigned char)d[i] == pad; if (same) cut.push_back(pad); } }
    cut.push_back(0);
    { size_t z = 0; while (z < 16 && z < d.size() && d[d.size() - 1 - z] == 0) ++z; for (size_t t = 1; t <= z; ++t) cut.push_back(t); }
    for (size_t c : cut) {
        if (d.size() < c + 10) continue;
        const size_t n = d.size() - c;
        uint32_t want = 0; memcpy(&want, d.data() + n - 4, 4);
        std::string y;
        if (inf::run((const uint8_t*)d.data() + 2, n - 6, y) && y.size() == want) { yaml.swap(y); return true; }
    }
    yaml.clear(); err = "wrong key or damaged save (no layout matched)"; return false;
}
static bool write_yaml(const fs::path& f, uint64_t sid, const std::string& yaml, std::string& err) {
    std::string d = zlib_store(yaml);
    const uint32_t n = (uint32_t)yaml.size(); d.append((const char*)&n, 4);
    const size_t pad = 16 - d.size() % 16; d.append(pad, (char)pad);
    uint8_t k[32]; key_for(sid, k);
    if (!aes(true, k, d)) { err = "encrypt failed"; return false; }
    if (d.empty() || d.size() % 16) { err = "encrypt failed (size)"; return false; }
    return write_file_atomic(f, d, err);
}

// ------------------------------------------------------------------ minimal YAML (the game's own layout: 2-space indents, "key: value")
struct Doc {
    std::vector<std::string> L;
    explicit Doc(const std::string& y) { std::string cur; for (char c : y) { if (c == '\n') { L.push_back(cur); cur.clear(); } else if (c != '\r') cur.push_back(c); } if (!cur.empty()) L.push_back(cur); }
    std::string text() const { std::string o; for (auto& l : L) { o += l; o += '\n'; } return o; }
    static int ind(const std::string& l) { int i = 0; while (i < (int)l.size() && l[i] == ' ') ++i; return i; }
    int end_of(int i) const {   // first line after the block that starts at i
        const int base = ind(L[i]);
        int j = i + 1;
        while (j < (int)L.size()) { const int d = ind(L[j]); if (L[j].find_first_not_of(' ') == std::string::npos) { ++j; continue; }
                                    if (d > base || (d == base && L[j][d] == '-')) ++j; else break; }
        return j;
    }
    int child(int parent, const std::string& key) const {   // parent -1 = top level
        const int b = parent < 0 ? 0 : parent + 1, e = parent < 0 ? (int)L.size() : end_of(parent);
        const int want = parent < 0 ? 0 : ind(L[parent]) + 2;
        for (int i = b; i < e; ++i) if (ind(L[i]) == want && L[i].compare(want, key.size() + 1, key + ":") == 0) return i;
        return -1;
    }
    int path(const std::string& p) const {
        int cur = -1; size_t s = 0;
        while (s <= p.size()) { size_t e = p.find('/', s); if (e == std::string::npos) e = p.size();
                                cur = child(cur, p.substr(s, e - s)); if (cur < 0) return -1; s = e + 1; }
        return cur;
    }
    std::string value(const std::string& p) const {
        const int i = path(p); if (i < 0) return "";
        const size_t c = L[i].find(':'); std::string v = c == std::string::npos ? "" : L[i].substr(c + 1);
        while (!v.empty() && v.front() == ' ') v.erase(v.begin()); while (!v.empty() && v.back() == ' ') v.pop_back();
        return v;
    }
    bool set(const std::string& p, const std::string& v) {   // creates the last key under an existing parent
        const int i = path(p);
        if (i >= 0) { const size_t c = L[i].find(':'); L[i] = L[i].substr(0, c + 1) + " " + v; return true; }
        const size_t sl = p.rfind('/'); if (sl == std::string::npos) return false;
        const int par = path(p.substr(0, sl)); if (par < 0) return false;
        L.insert(L.begin() + par + 1, std::string(ind(L[par]) + 2, ' ') + p.substr(sl + 1) + ": " + v);
        return true;
    }
};

static std::string quote(const std::string& s) { std::string o = "'"; for (char c : s) { if (c == '\'') o += "''"; else o += c; } return o + "'"; }

// ------------------------------------------------------------------ the editable fields
// editable = false: shown in the summary, but not accepted as an edit. The three point pools are recomputed by the game
// when it loads the save (HANDOFF 2026-11-10: no effect; SDU tokens live in profile.sav) - BL.POINTS changes them in game.
// lo / hi: allowed range of an int field. Edits only ever change a key the save already has; nothing is added to a save.
struct Field { const char* key; const char* path; const char* label; const char* kind; long long lo, hi; bool editable; };
static const Field kFields[] = {
    { "skill_points",  "progression/point_pools/characterprogresspoints", "Unspent skill points", "int", 0, 100000000, false },
    { "spec_tokens",   "progression/point_pools/specializationtokenpool", "Specialization tokens", "int", 0, 100000000, false },
    { "sdu_tokens",    "progression/point_pools/echotokenprogresspoints", "SDU tokens", "int", 0, 100000000, false },
    { "difficulty",    "state/player_difficulty", "Difficulty (Easy / Normal / Hard)", "enum", 0, 0, true },
    { "uvh_level",     "globals/vault_hunter_level", "UVH level (0-5)", "int", 0, 5, true },
    { "uvh_unlocked",  "globals/highest_unlocked_vault_hunter_level", "Highest unlocked UVH level", "int", 0, 5, true },
    { "mayhem_unlocked","globals/highest_unlocked_mayhem_level", "Highest unlocked Mayhem level", "int", 0, 10, true },
};
static const Field* field(const std::string& k) { for (auto& f : kFields) if (k == f.key) return &f; return nullptr; }

// json helpers that never throw (the queue file can be edited by hand)
static std::string jstr(const json& o, const char* k) {
    if (!o.is_object()) return std::string();
    const auto it = o.find(k);
    return it != o.end() && it->is_string() ? it->get<std::string>() : std::string();
}
static std::string after(const std::string& s, size_t pos) { return pos < s.size() ? s.substr(pos) : std::string(); }
static bool plain_sav_name(const std::string& f) {   // "4.sav", "profile.sav": a file name only, never a path
    if (f.size() < 5 || f.size() > 64 || f.compare(f.size() - 4, 4, ".sav") != 0 || f.find("..") != std::string::npos) return false;
    for (char c : f) if (!(isalnum((unsigned char)c) || c == '_' || c == '-' || c == '.')) return false;
    return true;
}
static fs::path sav_path(const fs::path& dir, const std::string& file) { return dir / fs::path(std::wstring(file.begin(), file.end())); }   // file: plain ASCII name
static bool has_children(const Doc& d, int i) {   // the key at line i holds a block (not a plain value)
    const int base = Doc::ind(d.L[i]);
    for (int j = i + 1; j < (int)d.L.size(); ++j) {
        if (d.L[j].find_first_not_of(' ') == std::string::npos) continue;
        const int k = Doc::ind(d.L[j]);
        return k > base || (k == base && d.L[j][k] == '-');
    }
    return false;
}
static bool in_block(int i, int b, int e) { return b >= 0 && i > b && i < e; }

// .tmp + rename so a crash cannot leave half a file.
static std::recursive_mutex g_pend_mtx;
static fs::path pending_path() { return fs::path(g_cfg.root) / "save_pending.json"; }
static json pending() {
    std::lock_guard<std::recursive_mutex> lk(g_pend_mtx);
    json j;
    { std::ifstream f(pending_path()); if (f) j = json::parse(f, nullptr, false); }
    if (!j.is_object()) j = json::object();
    json ops = json::array();
    if (j.contains("ops") && j["ops"].is_array()) for (auto& o : j["ops"]) if (o.is_object()) ops.push_back(o);
    j["ops"] = ops;
    if (j.contains("file") && !j["file"].is_string()) j.erase("file");
    return j;
}
static bool save_pending(const json& j) {
    std::lock_guard<std::recursive_mutex> lk(g_pend_mtx);
    std::string err;
    if (write_file_atomic(pending_path(), j.dump(1, ' ', false, json::error_handler_t::replace), err)) return true;
    log("[save] could not write save_pending.json: %s", err.c_str());
    return false;
}

static std::string selected_char(const Where& w, std::string& err) {   // profile.sav -> "characters_selected: C_4" -> 4.sav
    std::string y; if (!read_yaml(w.dir / L"profile.sav", w.steamid, y, err)) return "";
    std::smatch m; if (std::regex_search(y, m, std::regex("characters_selected: C_(\\d+)"))) return m[1].str() + ".sav";
    err = "no selected character in profile.sav"; return "";
}

static json summary(const Doc& d) {
    json s = { {"name", d.value("state/char_name")}, {"class", d.value("state/class")}, {"difficulty", d.value("state/player_difficulty")} };
    { const int e = d.path("state/experience"); if (e >= 0) for (int i = e + 1; i < d.end_of(e); ++i) if (d.L[i].find("level:") != std::string::npos) { s["level"] = atoi(d.L[i].c_str() + d.L[i].find(':') + 1); break; } }
    for (auto& f : kFields) { const std::string v = d.value(f.path); s[f.key] = v.empty() ? json() : json(v); }
    int items = 0; const int bp = d.path("state/inventory/items/backpack");
    if (bp >= 0) for (int i = bp + 1; i < d.end_of(bp); ++i) if (d.L[i].find("serial:") != std::string::npos) ++items;
    s["backpack_items"] = items;
    return s;
}

// done: one line per op for the log / reply.  kept: ops that could not be applied to this save now and stay queued.
// Returns how many ops changed the document (0 = the save is not written at all).
static int apply_ops(Doc& d, const json& ops, json& done, json& kept) {
    int applied = 0;
    for (auto& op : ops) {
        if (!op.is_object()) continue;
        const std::string o = jstr(op, "op");
        if (o == "add_item") {
            const std::string ser = jstr(op, "serial");
            if (!serial::round_trips(ser)) { done.push_back("dropped: '" + ser + "' is not a valid item serial"); continue; }
            int bp = d.path("state/inventory/items/backpack");
            if (bp < 0) { const int items = d.path("state/inventory/items"); if (items < 0) { done.push_back("kept in the queue: this save has no inventory yet"); kept.push_back(op); continue; }
                          d.L.insert(d.L.begin() + items + 1, std::string(Doc::ind(d.L[items]) + 2, ' ') + "backpack: "); bp = items + 1; }
            int maxslot = -1; const int e = d.end_of(bp), ci = Doc::ind(d.L[bp]) + 2;
            for (int i = bp + 1; i < e; ++i) if (Doc::ind(d.L[i]) == ci && d.L[i].compare(ci, 5, "slot_") == 0) maxslot = std::max(maxslot, atoi(d.L[i].c_str() + ci + 5));
            const std::string pad(ci, ' ');
            const int at = e;
            d.L.insert(d.L.begin() + at, { pad + "slot_" + std::to_string(maxslot + 1) + ": ", pad + "  serial: " + quote(ser), pad + "  flags: 1", pad + "  state_flags: 1" });
            done.push_back("item -> backpack slot " + std::to_string(maxslot + 1));
            ++applied;
        } else if (o == "replace_item") {   // the lines of the inventory that carry the old serial (backpack + equipped copy)
            const std::string olds = jstr(op, "old"), news = jstr(op, "new");
            if (olds.rfind("@U", 0) != 0 || !serial::round_trips(news)) { done.push_back("dropped: replace with an invalid serial"); continue; }
            const std::string from = "serial: " + quote(olds), to = "serial: " + quote(news);
            const int inv = d.path("state/inventory");
            const int bp = d.path("state/inventory/items/backpack"), eq = d.path("state/inventory/equipped_inventory");
            const int bpe = bp >= 0 ? d.end_of(bp) : -1, eqe = eq >= 0 ? d.end_of(eq) : -1;
            std::vector<int> hit; int nb = 0, ne = 0;
            if (inv >= 0) for (int i = inv + 1, e = d.end_of(inv); i < e; ++i) if (d.L[i].find(from) != std::string::npos) {
                hit.push_back(i); if (in_block(i, bp, bpe)) ++nb; else if (in_block(i, eq, eqe)) ++ne; }
            if (hit.empty()) {   // sold / dropped / changed since it was queued: not "applied" - it stays queued and the user is told
                done.push_back("replace: the original item is not in the save - kept in the queue (clear the queue if you no longer have it)");
                kept.push_back(op); continue; }
            // Several identical items in the backpack and none of them equipped: change one. With an equipped copy the save
            // does not say which backpack entry it belongs to, so every copy changes together (backpack and equipped stay in step).
            const bool one = nb > 1 && ne == 0;
            int n = 0;
            for (int i : hit) {
                if (one && (n >= 1 || !in_block(i, bp, bpe))) continue;
                const size_t k = d.L[i].find(from); if (k == std::string::npos) continue;
                d.L[i].replace(k, from.size(), to); ++n; }
            done.push_back("replace: " + std::to_string(n) + " line(s)" + (one ? " (one of " + std::to_string(nb) + " identical items)" : std::string()));
            if (n > 0) ++applied; else kept.push_back(op);
        } else if (o == "set") {
            const std::string key = jstr(op, "key");
            const Field* f = field(key);
            if (!f || !f->editable) { done.push_back("dropped: '" + key + "' is not an editable field"); continue; }
            if (!op.contains("value")) { done.push_back("dropped: " + key + " has no value"); continue; }
            const json& jv = op["value"];
            std::string v;
            if (std::string(f->kind) == "int") {
                long long n = 0;
                if (jv.is_number()) n = jv.get<long long>(); else if (jv.is_string()) n = atoll(jv.get<std::string>().c_str());
                else { done.push_back("dropped: " + key + " needs a number"); continue; }
                if (n < f->lo || n > f->hi) { done.push_back("dropped: " + key + " out of range"); continue; }
                v = std::to_string(n);
            } else {
                v = jv.is_string() ? jv.get<std::string>() : std::string();
                if (v != "Easy" && v != "Normal" && v != "Hard") { done.push_back("dropped: " + key + " must be Easy, Normal or Hard"); continue; }
            }
            const int i = d.path(f->path);
            if (i < 0 || has_children(d, i)) { done.push_back("dropped: this save has no '" + key + "' value (nothing is added to a save)"); continue; }
            if (!d.set(f->path, v)) { done.push_back("could not set " + key); continue; }
            done.push_back(key + " = " + v);
            ++applied;
        } else done.push_back("dropped: unknown edit '" + o + "'");
    }
    return applied;
}

static std::string stamp_now() { char stamp[32] = {}; time_t t = time(nullptr); tm lt{}; localtime_s(&lt, &t); strftime(stamp, sizeof stamp, "%Y%m%d_%H%M%S", &lt); return stamp; }
static std::string jtext(const json& j) { return j.dump(-1, ' ', false, json::error_handler_t::replace); }

// After an apply: the applied queue is archived (save_applied_<time>.json) and only the ops in `kept` stay queued.
// Applied edits must never stay in the queue (they would be applied again at the next start: duplicate items).
static void requeue(const json& pend, const json& kept, const std::string& stamp, bool archive) {
    std::error_code ec;
    if (archive) fs::rename(pending_path(), fs::path(g_cfg.root) / ("save_applied_" + stamp + ".json"), ec);
    json p = pend.is_object() ? pend : json(json::object()); p["ops"] = kept;
    if (!kept.empty()) { save_pending(p); return; }
    ec.clear(); fs::remove(pending_path(), ec); ec.clear();
    if (fs::exists(pending_path(), ec)) save_pending(p);   // could not be removed: at least empty it
}

static json apply_now(const json& pend, bool from_start) {
    std::lock_guard<std::recursive_mutex> lk(g_pend_mtx);   // one apply at a time, and the queue does not change under it
    Where w = where(); if (w.dir.empty()) return fail("NO_SAVES", "save folder not found");
    const std::string file = jstr(pend, "file");
    if (file.empty() || !pend.contains("ops") || !pend["ops"].is_array() || pend["ops"].empty()) return fail("EMPTY", "nothing queued");
    if (!plain_sav_name(file)) return fail("FILE", "the queued file name is not a character save: " + file);
    // the edits were queued for one account + character: never write them into another one
    const std::string sid = jstr(pend, "steamid");
    if (!sid.empty() && sid != std::to_string(w.steamid)) return fail("OTHER_ACCOUNT", "the queued edits belong to another Steam account's saves - nothing changed (clear the queue to drop them)");
    const fs::path f = sav_path(w.dir, file);
    std::string y, err; if (!read_yaml(f, w.steamid, y, err)) return fail("READ", err);
    Doc d(y);
    const std::string guid = jstr(pend, "char_guid");
    if (!guid.empty() && d.value("state/char_guid") != guid)
        return fail("OTHER_CHAR", "the queued edits were made for another character than the one in " + file + " - nothing changed (clear the queue to drop them)");
    json done = json::array(), kept = json::array();
    const int applied = apply_ops(d, pend["ops"], done, kept);
    const std::string stamp = stamp_now();
    if (applied == 0) {   // nothing to write: the save is not touched; ops that may still work later stay queued
        requeue(pend, kept, stamp, false);
        log("[save] no queued edit could be applied to %s: %s", file.c_str(), jtext(done).c_str());
        return fail("NOTHING_APPLIED", "no queued edit could be applied - the save was not changed: " + jtext(done));
    }
    std::error_code ec; const fs::path bk = fs::path(g_cfg.root) / "save_backups"; fs::create_directories(bk, ec); ec.clear();
    const fs::path bkf = bk / (file + "." + stamp);
    fs::copy_file(f, bkf, fs::copy_options::overwrite_existing, ec);
    if (ec) return fail("BACKUP", "backup failed - nothing changed: " + ec.message());
    {   // the backup must be the very content that was edited (the game may have written the save in between)
        std::string yb, berr;
        if (!read_yaml(bkf, w.steamid, yb, berr) || yb != y) return fail("BACKUP", "the save changed while it was being read - nothing changed, the edits stay queued");
    }
    const std::string out = d.text();
    if (!write_yaml(f, w.steamid, out, err)) return fail("WRITE", err);
    std::string chk, rerr; const bool rd = read_yaml(f, w.steamid, chk, rerr);
    if (!rd || chk != out) {
        const std::string why = rd ? std::string("the written save reads back differently") : "the written save could not be read back (" + rerr + ")";
        std::string orig, werr;
        const bool restored = read_file(bkf, orig) && !orig.empty() && write_file_atomic(f, orig, werr);
        if (restored) return fail("VERIFY", why + " - original restored, the edits stay queued");
        // The original could not be put back, so the edited file is what is on the disk: the same edits must not be applied
        // again at the next start.
        requeue(pend, kept, stamp, true);
        log("[save] VERIFY failed and the original could not be restored (%s); backup: %s", werr.c_str(), utf8(bkf).c_str());
        return fail("VERIFY", why + " and the original could NOT be restored (" + werr + ") - the backup is " + utf8(bkf));
    }
    requeue(pend, kept, stamp, true);
    log("[save] %s: applied %d edit(s) to %s (backup %s), %zu kept in the queue", from_start ? "game start" : "now", applied, file.c_str(), stamp.c_str(), kept.size());
    return ok({ {"file", file}, {"done", done}, {"backup", utf8(bkf)}, {"kept", kept.size()} });
}


// read-only mission log from the character save (the game writes it when it saves): tracked missions + every mission set
// with its status and missions. Never written by the trainer.
json missions_log() {
    Where w = where(); if (w.dir.empty()) return fail("NO_SAVES", "save folder not found");
    std::string err; const std::string file = selected_char(w, err); if (file.empty()) return fail("READ", err);
    std::string y; if (!read_yaml(sav_path(w.dir, file), w.steamid, y, err)) return fail("READ", err);
    Doc d(y);
    json out = { {"file", file}, {"tracked", json::array()}, {"sets", json::array()} };
    const int tr = d.path("missions/tracked_missions");
    if (tr >= 0) for (int i = tr + 1; i < d.end_of(tr); ++i) { std::string v = d.L[i]; const size_t k = v.find("- "); if (k != std::string::npos) out["tracked"].push_back(v.substr(k + 2)); }
    const int ls = d.path("missions/local_sets");
    if (ls >= 0) {
        const int si = Doc::ind(d.L[ls]) + 2;
        for (int i = ls + 1; i < d.end_of(ls); ++i) {
            if (Doc::ind(d.L[i]) != si || d.L[i].find(':') == std::string::npos) continue;
            const std::string set = d.L[i].substr(si, d.L[i].find(':') - si);
            json sj = { {"set", set}, {"status", ""}, {"missions", json::array()} };
            const int e = d.end_of(i);
            for (int k = i + 1; k < e; ++k) {
                const int ik = Doc::ind(d.L[k]);
                if (ik >= (int)d.L[k].size()) continue;   // blank line
                if (ik == si + 2 && d.L[k].compare(ik, 7, "status:") == 0) sj["status"] = after(d.L[k], (size_t)ik + 8);
                if (ik == si + 4 && d.L[k].find(':') != std::string::npos && d.L[k].back() == ' ') {   // a mission block
                    const std::string m = d.L[k].substr(ik, d.L[k].find(':') - ik); std::string st;
                    for (int q = k + 1; q < d.end_of(k); ++q) if (Doc::ind(d.L[q]) == ik + 2 && d.L[q].compare(ik + 2, 7, "status:") == 0) { st = after(d.L[q], (size_t)ik + 10); break; }
                    sj["missions"].push_back({ {"mission", m}, {"status", st} });
                }
            }
            out["sets"].push_back(sj);
        }
    }
    return ok(out);
}

static HWND game_window() {   // this process's visible UnrealWindow (nullptr = not created / not shown yet)
    std::pair<DWORD, HWND> look{ GetCurrentProcessId(), nullptr };
    EnumWindows([](HWND h, LPARAM p) -> BOOL { auto* L = (std::pair<DWORD, HWND>*)p; DWORD pid = 0; GetWindowThreadProcessId(h, &pid); char cls[64];
        if (pid == L->first && IsWindowVisible(h) && GetClassNameA(h, cls, 64) && strcmp(cls, "UnrealWindow") == 0) { L->second = h; return FALSE; } return TRUE; }, (LPARAM)&look);
    return look.second;
}

// at DLL start: the game has not read the save yet while the process is young.
// The window is only an estimate, so it is kept tight: edits are written when the process is at most 20 s old (the loader
// attaches within a few seconds of the game starting), or up to 45 s old while the game window is not on screen yet (slow
// start, still before the title screen that loads the character). A later attach (loader started by hand while the game
// is already at the title screen) writes nothing and leaves the queue for the next start - before, such an apply was
// overwritten by the game and still reported as done.
void apply_pending_at_start() {
    try {
        std::error_code ec; if (!fs::exists(pending_path(), ec)) return;
        FILETIME c, x, k, u; if (!GetProcessTimes(GetCurrentProcess(), &c, &x, &k, &u)) return;
        FILETIME now; GetSystemTimeAsFileTime(&now);
        const double age = ((ULARGE_INTEGER{ now.dwLowDateTime, now.dwHighDateTime }.QuadPart) - (ULARGE_INTEGER{ c.dwLowDateTime, c.dwHighDateTime }.QuadPart)) / 1e7;
        const bool shown = game_window() != nullptr;
        if (age > 45 || (age > 20 && shown)) { log("[save] edits are queued but the game has run %.0f s%s - they apply at the next game start", age, shown ? " and its window is up" : ""); return; }
        json r = apply_now(pending(), true);
        log("[save] %s", jtext(r).c_str());
    } catch (std::exception& e) { log("[save] queued edits were not applied (%s)", e.what()); }
      catch (...) { log("[save] queued edits were not applied (unknown error)"); }
}

static json cmd_inner(const std::string& name, const json& a) {
    Where w = where(); if (w.dir.empty()) return fail("NO_SAVES", "Borderlands 4 save folder not found");
    std::string err;
    if (name == "TQ.SAVE" || name == "TQ.SAVE_INFO") {
        std::string file = jstr(a, "file"); if (file.empty()) file = selected_char(w, err);
        if (file.empty()) return fail("READ", err);
        if (!plain_sav_name(file)) return fail("FILE", "file must be a save file name like 4.sav");
        std::string y; if (!read_yaml(sav_path(w.dir, file), w.steamid, y, err)) return fail("READ", err);
        const json pend = pending();
        json s = summary(Doc(y)); s["file"] = file; s["folder"] = utf8(w.dir); s["pending"] = pend["ops"];
        if (a.is_object() && a.contains("dump") && a["dump"].is_boolean() && a["dump"].get<bool>()) {
            const fs::path out = fs::path(g_cfg.root) / ("save_" + file + ".yaml"); std::ofstream(out) << y; s["dump"] = utf8(out); }
        const std::string lvl = s.contains("level") ? std::to_string(s["level"].get<int>()) : "?";
        return ok(s, s.value("name", "?") + " lvl " + lvl + " (" + file + "), " + std::to_string(pend["ops"].size()) + " edit(s) queued");
    }
    if (name == "TQ.SAVE_QUEUE") {   // {op:"add_item", serial} | {op:"set", key, value} | {serials:"a b c"} | {key, v}
        std::lock_guard<std::recursive_mutex> lk(g_pend_mtx);
        json p = pending();
        const std::string file = selected_char(w, err); if (file.empty()) return fail("READ", err);
        const std::string pf = jstr(p, "file");
        if (!p["ops"].empty() && !pf.empty() && pf != file) return fail("OTHER_CHAR", "edits for " + pf + " are still queued - clear them first");
        // the character these edits are for: its guid goes into the queue and is checked again before anything is written
        std::string y; if (!read_yaml(sav_path(w.dir, file), w.steamid, y, err)) return fail("READ", err);
        const Doc d(y);
        const std::string guid = d.value("state/char_guid"), pg = jstr(p, "char_guid");
        if (!p["ops"].empty() && !pg.empty() && !guid.empty() && pg != guid) return fail("OTHER_CHAR", "edits for another character are still queued - clear them first");
        p["file"] = file; p["steamid"] = std::to_string(w.steamid);
        if (!guid.empty()) p["char_guid"] = guid; else p.erase("char_guid");
        int added = 0, bad = 0;
        auto add_serials = [&](const std::string& text) {
            std::regex re("@U[^\\s'\",]+");
            for (auto it = std::sregex_iterator(text.begin(), text.end(), re); it != std::sregex_iterator(); ++it) {
                const std::string s = it->str();
                if (!serial::round_trips(s)) { ++bad; continue; }   // a cut-off or mistyped serial never reaches the save
                p["ops"].push_back({ {"op", "add_item"}, {"serial", s} }); ++added; } };
        const std::string opn = jstr(a, "op");
        if (a.contains("serial") && a["serial"].is_string() && opn != "replace_item") add_serials(a["serial"].get<std::string>());
        if (a.contains("serials")) add_serials(a["serials"].is_string() ? a["serials"].get<std::string>() : a["serials"].dump());
        if (opn == "add_item" && a.contains("v")) add_serials(a["v"].is_string() ? a["v"].get<std::string>() : "");
        if (opn == "replace_item") {
            const std::string o = jstr(a, "old"), nw = jstr(a, "new");
            if (o.rfind("@U", 0) != 0 || nw.rfind("@U", 0) != 0) return fail("SERIAL", "old and new serials needed");
            if (!serial::round_trips(nw)) return fail("SERIAL", "the new serial does not decode back the same - not queued");
            // one replace per original item: the newest choice wins (an older queued replace of the same item is dropped)
            for (auto it = p["ops"].begin(); it != p["ops"].end();) { if (jstr(*it, "op") == "replace_item" && jstr(*it, "old") == o) it = p["ops"].erase(it); else ++it; }
            if (o == nw) {   // back to the original: nothing left to do for this item
                if (!save_pending(p)) return fail("IO", "could not write the queue file");
                return ok(p, "the item is unchanged - nothing queued for it (" + std::to_string(p["ops"].size()) + " total)"); }
            p["ops"].push_back({ {"op", "replace_item"}, {"old", o}, {"new", nw} }); ++added;
        }
        if (a.contains("key")) {
            const Field* f = field(jstr(a, "key")); if (!f) return fail("FIELD", "unknown field");
            if (!f->editable) return fail("FIELD", std::string(f->label) + ": the game works this out again when it loads the save, so a save edit has no effect (change it in game)");
            { const int i = d.path(f->path); if (i < 0 || has_children(d, i)) return fail("FIELD", "this save has no such value - nothing is added to a save"); }
            json v = a.contains("value") ? a["value"] : a.contains("v") ? a["v"] : json();
            if (v.is_null()) return fail("VALUE", "no value");
            if (std::string(f->kind) == "int") {
                long long n = 0;
                if (v.is_number()) n = v.get<long long>(); else if (v.is_string()) n = atoll(v.get<std::string>().c_str()); else return fail("VALUE", "a number is needed");
                if (n < f->lo || n > f->hi) return fail("VALUE", "out of range (" + std::to_string(f->lo) + " .. " + std::to_string(f->hi) + ")");
                v = n; }
            else { std::string s = v.is_string() ? v.get<std::string>() : v.dump(); if (s != "Easy" && s != "Normal" && s != "Hard") return fail("VALUE", "Easy, Normal or Hard"); v = s; }
            for (auto it = p["ops"].begin(); it != p["ops"].end();) { if (jstr(*it, "op") == "set" && jstr(*it, "key") == f->key) it = p["ops"].erase(it); else ++it; }
            p["ops"].push_back({ {"op", "set"}, {"key", f->key}, {"value", v} }); ++added;
        }
        if (!added) return bad ? fail("SERIAL", "that is not a valid item serial (it does not decode) - not queued")
                               : fail("NOTHING", "paste one or more serials (they start with @U) or give key + value");
        if (!save_pending(p)) return fail("IO", "could not write the queue file");
        return ok(p, std::to_string(added) + " queued (" + std::to_string(p["ops"].size()) + " total)" + (bad ? ", " + std::to_string(bad) + " invalid serial(s) skipped" : std::string()) + " - applied when the game starts again");
    }
    if (name == "TQ.SAVE_MISSIONS") return missions_log();
    if (name == "TQ.SAVE_CLEAR") { std::lock_guard<std::recursive_mutex> lk(g_pend_mtx); std::error_code ec; fs::remove(pending_path(), ec); return ok(json::object(), "queued save edits cleared"); }
    if (name == "TQ.SAVE_APPLY") {   // offline use (game at the title screen will overwrite it - prefer SAVE_RESTART)
        if (!(a.is_object() && a.contains("force") && a["force"].is_boolean() && a["force"].get<bool>())) return fail("RUNNING", "the game overwrites the save while running - use Apply & restart");
        return apply_now(pending(), false);
    }
    if (name == "TQ.SAVE_RESTART") {   // close the game like clicking X; the loader waits, starts it again, the new DLL applies the edits
        if (pending()["ops"].empty()) return fail("EMPTY", "nothing queued");
        // find the window first: without it nothing is started (before, the loader was already waiting when this failed)
        HWND hw = game_window();
        if (!hw) return fail("WINDOW", "game window not found - quit the game yourself and start it again; the queued edits apply at the next start");
        // 1.1.0: no trainer exe any more - the game closes like clicking X (it saves first); started again from Steam,
        // dsound.dll loads the trainer, which applies the queued edits before the game reads the save
        PostMessageW(hw, WM_CLOSE, 0, 0);
        toast("Closing the game - start it again from Steam, the save edits are applied when it starts");
        return ok(json::object(), "closing the game - start it again from Steam; the queued edits are applied when it starts");
    }
    return fail("UNKNOWN", name);
}

json cmd(const std::string& name, const json& a) {
    try { return cmd_inner(name, a); }
    catch (std::exception& e) { return fail("EXCEPTION", name + ": " + e.what()); }
    catch (...) { return fail("EXCEPTION", name + ": unknown error"); }
}

} // namespace wb::sv
