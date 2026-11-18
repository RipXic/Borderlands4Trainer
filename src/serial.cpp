// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// "@U" + base85 (custom alphabet) -> bytes, each byte bit-reversed -> bits: magic 0010000, then tokens
//   00 sep | 01 soft | 100 varint (4 bits + continue, LSB first) | 110 varbit (5-bit length + bits) | 111 string
//   101 part: varint index, then 1 + varint value + 000 | 0 + 10 (plain) | 0 + 01 + list (values / 01) + 00
// Item: <type> , 0 , 1 , <level> | 2 , <seed> | | {comp} {part} ...  ({cat:v} / {cat:[...]} = parts of a shared category)
#include "common.h"
#include "serial.h"

namespace wb::serial {
static const char* kAlph = "0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz!#$%&()*+-;<=>?@^_`{/}~";
static int a_idx(char c) { const char* p = strchr(kAlph, c); return p && c ? (int)(p - kAlph) : -1; }
static uint8_t rev8(uint8_t b) { uint8_t r = 0; for (int i = 0; i < 8; ++i) if (b & (1 << i)) r |= 1 << (7 - i); return r; }

static bool b85_decode(const std::string& s, std::vector<uint8_t>& out) {
    for (size_t i = 0; i < s.size(); i += 5) {
        const size_t n = std::min<size_t>(5, s.size() - i); uint64_t v = 0;
        if (n == 1) return false;                  // one left-over character carries no byte: not a serial
        for (size_t k = 0; k < 5; ++k) { const int d = k < n ? a_idx(s[i + k]) : 84; if (d < 0) return false; v = v * 85 + d; }
        if (v > 0xFFFFFFFFull) return false;       // a group above 2^32 never comes from the encoder
        v &= 0xFFFFFFFFull;
        const uint8_t b[4] = { (uint8_t)(v >> 24), (uint8_t)(v >> 16), (uint8_t)(v >> 8), (uint8_t)v };
        out.insert(out.end(), b, b + (n == 5 ? 4 : n - 1));
    }
    return true;
}
static std::string b85_encode(const std::vector<uint8_t>& b) {
    std::string o;
    for (size_t i = 0; i < b.size(); i += 4) {
        const size_t n = std::min<size_t>(4, b.size() - i); uint64_t v = 0;
        for (size_t k = 0; k < 4; ++k) v = (v << 8) | (k < n ? b[i + k] : 0);
        char d[5]; for (int k = 4; k >= 0; --k) { d[k] = kAlph[v % 85]; v /= 85; }
        o.append(d, n == 4 ? 5 : n + 1);
    }
    return o;
}

struct Rd {
    std::vector<uint8_t> b; size_t p = 0; bool bad = false;   // bad: a varint longer than 16 nibbles (malformed)
    size_t left() const { return p < b.size() ? b.size() - p : 0; }
    int bit() { return p < b.size() ? b[p++] : (p++, 0); }
    uint64_t n(int k) { uint64_t v = 0; for (int i = 0; i < k; ++i) v |= (uint64_t)bit() << i; return v; }
    std::string peek(int k) const { std::string s; for (int i = 0; i < k && p + i < b.size(); ++i) s += (char)('0' + b[p + i]); return s; }
    bool zeros() const { for (size_t i = p; i < b.size(); ++i) if (b[i]) return false; return true; }
    Val varint() { Val v; v.kind = Val::Int; int sh = 0; for (;;) { v.v |= n(4) << sh; sh += 4; ++v.width; if (!bit()) break; if (sh > 60) { bad = true; break; } } return v; }
    Val varbit() { Val v; v.kind = Val::Bit; v.width = (int)n(5); v.v = v.width ? n(v.width) : 0; return v; }
};
struct Wr {
    std::vector<uint8_t> b;
    void put(const char* s) { for (; *s; ++s) b.push_back(*s == '1'); }
    void n(uint64_t v, int k) { for (int i = 0; i < k; ++i) b.push_back((v >> i) & 1); }
    void val(const Val& v) {
        if (v.kind == Val::Int) {
            int need = 1; while (need < 16 && (v.v >> (4 * need))) ++need;
            const int w = std::max(need, v.width);
            put("100"); for (int i = 0; i < w; ++i) { n((v.v >> (4 * i)) & 15, 4); b.push_back(i < w - 1); }
        } else {
            int need = 0; while (need < 31 && (v.v >> need)) ++need;
            const int w = std::max(need, v.width); put("110"); n(w, 5); n(v.v, w);
        }
    }
};

bool decode(const std::string& serial, Item& it, std::string* err) {
    it = Item{};
    std::string s = serial; while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    if (s.rfind("@U", 0) != 0) { if (err) *err = "a serial starts with @U"; return false; }
    std::vector<uint8_t> raw; if (!b85_decode(s.substr(2), raw)) { if (err) *err = "bad character"; return false; }
    it.nbytes = (int)raw.size();
    Rd r; for (uint8_t x : raw) { const uint8_t y = rev8(x); for (int i = 7; i >= 0; --i) r.b.push_back((y >> i) & 1); }
    if (r.peek(7) != "0010000") { if (err) *err = "not an item serial (magic)"; return false; }
    r.p = 7;
    while (r.left() >= 2) {
        if (r.zeros()) break;
        Tok t; const std::string two = r.peek(2);
        if (two == "00") { r.p += 2; t.kind = Tok::Sep; }
        else if (two == "01") { r.p += 2; t.kind = Tok::Soft; }
        else {
            const std::string three = r.peek(3); r.p += 3;
            if (three == "100") { t.kind = Tok::Value; t.val = r.varint(); }
            else if (three == "110") { t.kind = Tok::Value; t.val = r.varbit(); }
            else if (three == "101") {
                t.kind = Tok::Part; t.idx = r.varint();
                if (r.bit()) { t.sub = Tok::One; t.one = r.varint(); if (r.peek(3) != "000") { if (err) *err = "part end"; return false; } r.p += 3; }
                else {
                    const std::string k = r.peek(2); r.p += 2;
                    if (k == "10") t.sub = Tok::Plain;
                    else if (k == "01") {
                        t.sub = Tok::List;
                        while (r.left() >= 2 && r.peek(2) != "00") {
                            if (r.peek(2) == "01") { r.p += 2; Val sv; sv.kind = Val::Soft; t.list.push_back(sv); continue; }
                            const std::string vt = r.peek(3); r.p += 3;
                            if (vt == "100") t.list.push_back(r.varint()); else if (vt == "110") t.list.push_back(r.varbit());
                            else { if (err) *err = "part list " + vt; return false; }
                        }
                        r.p += 2;
                    } else { if (err) *err = "part kind " + k; return false; }
                }
            } else if (three == "111") {
                t.kind = Tok::Str; const Val L = r.varint();
                if (L.v > 256) { if (err) *err = "string too long"; return false; }
                for (uint64_t i = 0; i < L.v; ++i) t.str.push_back((char)r.n(7));
            } else { if (err) *err = "token " + three; return false; }
        }
        it.toks.push_back(t);
        if (r.bad) { if (err) *err = "number too long"; return false; }
        if (r.p > r.b.size()) { if (err) *err = "truncated"; return false; }
    }
    return true;
}

std::string encode(const Item& it) {
    Wr w; w.put("0010000");
    for (const Tok& t : it.toks) {
        switch (t.kind) {
        case Tok::Sep: w.put("00"); break;
        case Tok::Soft: w.put("01"); break;
        case Tok::Value: w.val(t.val); break;
        case Tok::Str: {   // 111 + varint length (without its 100 prefix) + 7-bit chars
            w.put("111"); Val L; L.v = t.str.size(); Wr tmp; tmp.val(L); w.b.insert(w.b.end(), tmp.b.begin() + 3, tmp.b.end());
            for (char c : t.str) w.n((uint8_t)c, 7);
            break; }
        case Tok::Part: {
            w.put("101"); Wr tmp; tmp.val(t.idx); w.b.insert(w.b.end(), tmp.b.begin() + 3, tmp.b.end());
            if (t.sub == Tok::Plain) w.put("010");
            else if (t.sub == Tok::One) { w.put("1"); Wr t2; t2.val(t.one); w.b.insert(w.b.end(), t2.b.begin() + 3, t2.b.end()); w.put("000"); }
            else { w.put("001"); for (const Val& v : t.list) { if (v.kind == Val::Soft) w.put("01"); else w.val(v); } w.put("00"); }
            break; }
        }
    }
    w.put("00");
    while (w.b.size() % 8) w.b.push_back(0);
    std::vector<uint8_t> raw;
    for (size_t i = 0; i < w.b.size(); i += 8) { uint8_t y = 0; for (int k = 0; k < 8; ++k) y = (uint8_t)((y << 1) | w.b[i + k]); raw.push_back(rev8(y)); }
    if (it.nbytes > 0 && (int)raw.size() < it.nbytes) raw.resize(it.nbytes, 0);
    return "@U" + b85_encode(raw);
}

// header helpers: <type> , 0 , 1 , <level> | 2 , <seed> | |
static int nth_value(const Item& it, int n) { int k = 0; for (size_t i = 0; i < it.toks.size(); ++i) if (it.toks[i].kind == Tok::Value) { if (k == n) return (int)i; ++k; } return -1; }
int type_index(const Item& it) { const int i = nth_value(it, 0); return i < 0 ? -1 : (int)it.toks[i].val.v; }
// the level exists only in the header "<type> , 0 , 1 , <level> |" (real items also have e.g. "21 , 0 , 2 , 537 | |",
// where the 4th value is something else - never read or write that as a level)
int level_tok(const Item& it) {
    const auto& t = it.toks;
    if (t.size() < 8) return -1;
    if (t[0].kind != Tok::Value || t[1].kind != Tok::Soft || t[2].kind != Tok::Value || t[3].kind != Tok::Soft ||
        t[4].kind != Tok::Value || t[5].kind != Tok::Soft || t[6].kind != Tok::Value || t[7].kind != Tok::Sep) return -1;
    if (t[2].val.v != 0 || t[4].val.v != 1) return -1;
    return 6;
}
int level(const Item& it) { const int i = level_tok(it); return i < 0 ? -1 : (int)it.toks[i].val.v; }
bool set_level(Item& it, int lv) { const int i = level_tok(it); if (i < 0) return false; it.toks[i].val.v = (uint64_t)std::clamp(lv, 1, 99); return true; }
int first_part(const Item& it) { for (size_t i = 0; i < it.toks.size(); ++i) if (it.toks[i].kind == Tok::Part) return (int)i; return -1; }
int comp_part(const Item& it) {
    for (size_t i = 0; i < it.toks.size(); ++i) {
        if (it.toks[i].kind == Tok::Str) return -1;   // the comp is written by name: the first part is an ordinary part
        if (it.toks[i].kind == Tok::Part) return it.toks[i].sub == Tok::Plain ? (int)i : -1;
    }
    return -1;
}
bool round_trips(const std::string& serial) {
    std::string s = serial; while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n')) s.pop_back();
    Item it; return decode(s, it) && encode(it) == s;
}

std::string text(const Item& it) {
    std::string o;
    auto v = [](const Val& x) { return x.kind == Val::Bit ? "b" + std::to_string(x.v) : x.kind == Val::Soft ? std::string(",") : std::to_string(x.v); };
    for (const Tok& t : it.toks) {
        if (!o.empty()) o += ' ';
        if (t.kind == Tok::Sep) o += '|'; else if (t.kind == Tok::Soft) o += ',';
        else if (t.kind == Tok::Value) o += v(t.val);
        else if (t.kind == Tok::Str) o += "'" + t.str + "'";
        else { o += "{" + std::to_string(t.idx.v);
               if (t.sub == Tok::One) o += ":" + v(t.one);
               else if (t.sub == Tok::List) { o += ":["; for (size_t i = 0; i < t.list.size(); ++i) o += (i ? " " : "") + v(t.list[i]); o += "]"; }
               o += "}"; }
    }
    return o;
}
} // namespace wb::serial
