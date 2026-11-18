// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
#include "ue.h"
#include "MinHook.h"
#include <unordered_map>
#include <atomic>
#include <algorithm>
#include <fstream>
#include <map>
#include <mutex>
#include <shared_mutex>
#include <array>
#include <malloc.h>

namespace ue {

static std::string lower_ascii(std::string s) { for (auto& c : s) if (c >= 'A' && c <= 'Z') c = (char)(c - 'A' + 'a'); return s; }

// ------------------------------------------------------------------ guarded memory
// A read that lands on a thread's stack guard page raises STATUS_GUARD_PAGE_VIOLATION and removes the (one-shot) guard:
// swallowing it would leave that thread unable to grow its stack. The guard is put back before returning false.
static int seh_copy_filter(EXCEPTION_POINTERS* ep, DWORD* code, uintptr_t* addr) {
    *code = ep->ExceptionRecord->ExceptionCode;
    *addr = ep->ExceptionRecord->NumberParameters >= 2 ? (uintptr_t)ep->ExceptionRecord->ExceptionInformation[1] : 0;
    return EXCEPTION_EXECUTE_HANDLER;
}
static void rearm_guard_page(uintptr_t addr) {
    MEMORY_BASIC_INFORMATION mbi{};
    if (!addr || !VirtualQuery((void*)addr, &mbi, sizeof mbi) || mbi.State != MEM_COMMIT || (mbi.Protect & PAGE_GUARD)) return;
    DWORD old = 0; VirtualProtect((void*)(addr & ~(uintptr_t)0xFFF), 0x1000, mbi.Protect | PAGE_GUARD, &old);
}
static int seh_copy(void* dst, const void* src, size_t n) {
    DWORD code = 0; uintptr_t fault = 0;
    __try { memcpy(dst, src, n); return 1; }
    __except (seh_copy_filter(GetExceptionInformation(), &code, &fault)) { if (code == 0x80000001u) rearm_guard_page(fault); return 0; }
}
bool mread(uintptr_t a, void* out, size_t n) { if (a < 0x10000 || !out) return false; return seh_copy(out, (const void*)a, n) != 0; }
bool mwrite(uintptr_t a, const void* in, size_t n) { if (a < 0x10000 || !in) return false; return seh_copy((void*)a, in, n) != 0; }

// ------------------------------------------------------------------ state
static std::atomic<bool> g_ready{false};
static std::mutex g_init_mtx;
static uintptr_t g_base = 0; static size_t g_size = 0;
static uintptr_t g_text = 0; static size_t g_text_size = 0;
static uintptr_t g_data = 0; static size_t g_data_size = 0;   // .data (+bss): structure-scan fallback (BL4: patterns miss)
static uintptr_t g_objobjects = 0;      // GUObjectArray + 0x10 = FChunkedFixedUObjectArray
static uintptr_t g_namepool = 0;        // FNamePool (FNameEntryAllocator: lock, CurrentBlock +8, cursor +0xC, Blocks +0x10)
static uintptr_t g_process_event = 0;   // UObject::ProcessEvent
static int g_pe_slot = -1;              // its vtable slot (fallback when the pattern breaks after an update)
static int g_item_stride = 0x18;        // FUObjectItem size
static int g_item_obj = 8;              // where the UObject* sits inside FUObjectItem (UE 5.7: +8, UE <= 5.5: +0)
static uintptr_t g_cls_package = 0, g_cls_class = 0, g_cls_function = 0, g_cls_scriptstruct = 0;
static json g_found = json::object();
static std::string g_journal_dir;
static thread_local bool t_journal = false;
static std::vector<std::string> g_blocked_paths;

static constexpr int kChunk = 64 * 1024;

// ------------------------------------------------------------------ pattern scanning (in the loaded exe)
static bool text_section() {
    g_base = game_base(&g_size);
    if (!g_base) return false;
    IMAGE_DOS_HEADER dos; IMAGE_NT_HEADERS64 nt;
    if (!rd_as(g_base, dos) || !rd_as(g_base + dos.e_lfanew, nt)) return false;
    uintptr_t sh = g_base + dos.e_lfanew + offsetof(IMAGE_NT_HEADERS64, OptionalHeader) + nt.FileHeader.SizeOfOptionalHeader;
    for (int i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
        IMAGE_SECTION_HEADER s; if (!rd_as(sh + i * sizeof s, s)) return false;
        if (memcmp(s.Name, ".text", 5) == 0 && !g_text) { g_text = g_base + s.VirtualAddress; g_text_size = s.Misc.VirtualSize; }
        if (memcmp(s.Name, ".data", 5) == 0 && !g_data) { g_data = g_base + s.VirtualAddress; g_data_size = s.Misc.VirtualSize; }
    }
    return g_text != 0;
}

// all matches of pattern in .text (direct reads: .text is mapped and readable for the whole game session)
static std::vector<uintptr_t> scan(const std::string& pattern, size_t max) {
    std::vector<int> pat; std::vector<uintptr_t> out;
    if (!parse_pattern(pattern, pat) || pat.empty() || pat[0] < 0 || !g_text) return out;
    const uint8_t* p = (const uint8_t*)g_text; const size_t n = g_text_size, m = pat.size();
    const uint8_t first = (uint8_t)pat[0];
    for (size_t i = 0; i + m <= n; ) {
        const void* hit = memchr(p + i, first, n - m + 1 - i);
        if (!hit) break;
        i = (const uint8_t*)hit - p;
        bool okm = true;
        for (size_t k = 1; k < m; ++k) if (pat[k] >= 0 && p[i + k] != (uint8_t)pat[k]) { okm = false; break; }
        if (okm) { out.push_back(g_text + i); if (max && out.size() >= max) break; }
        ++i;
    }
    return out;
}

// rip-relative target most matches agree on: pattern hit + disp_at -> [disp32] -> next instruction + extra
static uintptr_t vote(const std::vector<std::string>& patterns, int disp_at, int extra, const char* what, size_t max_hits = 400) {
    std::map<uintptr_t, int> votes; int total = 0;
    for (auto& pat : patterns) {
        for (uintptr_t h : scan(pat, max_hits)) {
            int32_t d = 0; if (!rd_as(h + disp_at, d)) continue;
            ++votes[h + disp_at + 4 + extra + d]; ++total;
        }
    }
    uintptr_t best = 0; int bn = 0;
    for (auto& kv : votes) if (kv.second > bn) { bn = kv.second; best = kv.first; }
    g_found[what] = { {"rva", best ? hexstr(best - g_base) : "-"}, {"votes", bn}, {"matches", total}, {"candidates", votes.size()} };
    return best;
}

// ------------------------------------------------------------------ names (FNamePool, UE 5.5 layout)
static std::shared_mutex g_name_mtx;
static std::unordered_map<uint32_t, std::string> g_name_cache;
static std::unordered_map<std::string, uint32_t> g_name_rev;    // built on first name_find
static bool g_rev_built = false;
static uint32_t g_rev_block = 0, g_rev_off = 0;                     // incremental: FNamePool only ever appends
static Perf g_perf;
Perf& perf() { return g_perf; }
double now_ms() { static LARGE_INTEGER f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return x; }(); LARGE_INTEGER c; QueryPerformanceCounter(&c); return c.QuadPart * 1000.0 / f.QuadPart; }

static bool entry_at(uint32_t index, std::string& out) {
    const uint32_t block = index >> 16, offs = (index & 0xFFFF) * 2;
    uintptr_t blk = 0; if (!rd_as(g_namepool + 0x10 + block * 8, blk) || !blk) return false;
    uint16_t hdr = 0; if (!rd_as(blk + offs, hdr)) return false;
    const bool wide = hdr & 1; const uint32_t len = hdr >> 6;
    if (len == 0 || len > 1024) return false;
    if (!wide) { out.resize(len); return mread(blk + offs + 2, out.data(), len); }
    std::wstring w(len, L'\0'); if (!mread(blk + offs + 2, w.data(), len * 2)) return false;
    int u = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)len, nullptr, 0, nullptr, nullptr);
    out.resize(u); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)len, out.data(), u, nullptr, nullptr);
    return true;
}

std::string name_str(FName n) {
    std::string base;
    { std::shared_lock lk(g_name_mtx); auto it = g_name_cache.find(n.index); if (it != g_name_cache.end()) base = it->second; }
    if (base.empty()) {
        if (!g_namepool || !entry_at(n.index, base)) return "";
        std::unique_lock lk(g_name_mtx); if (g_name_cache.size() > 400000) g_name_cache.clear(); g_name_cache[n.index] = base;
    }
    if (n.number) base += "_" + std::to_string(n.number - 1);
    return base;
}

// Reverse name map, built INCREMENTALLY: the pool only appends, so after the first full pass a miss reads just the
// names added since (was: re-read the whole ~1M-name pool on every miss = long game-thread hitch; 2026-11-09).
static void build_rev() {
    const double t0 = now_ms();
    uint32_t cur_block = 0, cursor = 0;
    rd_as(g_namepool + 0x8, cur_block); rd_as(g_namepool + 0xC, cursor);
    uint32_t from_b = 0, from_o = 0; bool first = false;
    { std::shared_lock lk(g_name_mtx); from_b = g_rev_block; from_o = g_rev_off; first = !g_rev_built; }
    std::vector<std::pair<std::string, uint32_t>> add; if (first) add.reserve(1 << 20);
    static thread_local std::vector<uint8_t> blockbuf(0x20000);
    static thread_local std::wstring wtmp;
    uint32_t b = from_b, start = from_o, end_b = from_b, end_o = from_o;
    for (; b <= cur_block && b < 8192; ++b, start = 0) {
        uintptr_t blk = 0; if (!rd_as(g_namepool + 0x10 + b * 8, blk) || !blk) break;
        const bool cur = b == cur_block;
        uint32_t limit = cur ? cursor : 0x20000; if (limit > 0x20000) limit = 0x20000;   // never past the block / our buffer
        if (limit <= start) { end_b = b; end_o = start; continue; }
        if (!mread(blk + start, blockbuf.data() + start, limit - start)) break;
        // The tail of the current block may hold entries another thread is still writing (header first, chars later):
        // they are added, but the scan position stays before them so the next pass reads them again.
        const uint32_t stable = cur ? (limit > 0x1000 ? limit - 0x1000 : 0) : limit;
        uint32_t o = start, commit = start;
        for (; o + 2 <= limit; ) {
            if (o <= stable) commit = o;
            uint16_t hdr; memcpy(&hdr, blockbuf.data() + o, 2);
            const uint32_t len = hdr >> 6; const bool wide = hdr & 1;
            if (len == 0) break;
            const uint32_t bytes = len * (wide ? 2 : 1);
            if (o + 2 + bytes > limit) break;
            if (!wide) add.emplace_back(lower_ascii(std::string((const char*)blockbuf.data() + o + 2, len)), (b << 16) | (o / 2));
            else {
                wtmp.assign(len, L'\0'); memcpy(wtmp.data(), blockbuf.data() + o + 2, bytes);
                const int u = WideCharToMultiByte(CP_UTF8, 0, wtmp.data(), (int)len, nullptr, 0, nullptr, nullptr);
                if (u > 0) { std::string s8(u, '\0'); WideCharToMultiByte(CP_UTF8, 0, wtmp.data(), (int)len, s8.data(), u, nullptr, nullptr);
                             add.emplace_back(lower_ascii(std::move(s8)), (b << 16) | (o / 2)); }
            }
            o += (2 + bytes + 1) & ~1u;
        }
        if (o <= stable) commit = o;
        end_b = b; end_o = cur ? commit : o;
        if (!cur) { end_b = b + 1; end_o = 0; }
    }
    std::unique_lock lk(g_name_mtx);
    if (!g_rev_built) g_name_rev.reserve(add.size() + (1 << 16));
    for (auto& kv : add) g_name_rev.emplace(std::move(kv.first), kv.second);
    if (end_b > g_rev_block || (end_b == g_rev_block && end_o > g_rev_off)) { g_rev_block = end_b; g_rev_off = end_o; }
    g_rev_built = true;
    ++g_perf.name_builds; g_perf.name_ms += now_ms() - t0;
}

// Every FName a text can stand for (0..2): the whole text as one entry, and "Foo_3" as "Foo" + number 4 (suffix without
// a leading zero, or exactly "0"). Names are case-insensitive: the reverse map is keyed in lower case.
static int name_find_all(const std::string& s_in, FName out[2]) {
    const std::string s = lower_ascii(s_in);
    std::string b2; uint32_t num2 = 0; bool can_split = false;
    const size_t us = s.find_last_of('_');
    if (us != std::string::npos && us > 0 && us + 1 < s.size() && (s[us + 1] != '0' || us + 2 == s.size()) &&
        s.find_first_not_of("0123456789", us + 1) == std::string::npos && s.size() - us <= 10) {
        can_split = true; b2 = s.substr(0, us); num2 = (uint32_t)strtoul(s.c_str() + us + 1, nullptr, 10) + 1;
    }
    for (int pass = 0; pass < 2; ++pass) {
        int n = 0;
        { std::shared_lock lk(g_name_mtx);
          auto it = g_name_rev.find(s);
          if (it != g_name_rev.end()) { out[n].index = it->second; out[n].number = 0; ++n; }
          if (can_split) { it = g_name_rev.find(b2); if (it != g_name_rev.end()) { out[n].index = it->second; out[n].number = num2; ++n; } } }
        if (n || pass == 1 || !g_namepool) return n;
        build_rev();   // new names appear while the game runs: read what was added and retry once
    }
    return 0;
}
bool name_find(const std::string& s, FName& out) {
    FName c[2]; if (!name_find_all(s, c)) return false;
    out = c[0]; return true;
}

// ------------------------------------------------------------------ objects
int32_t num_objects() { int32_t n = 0; if (g_objobjects) rd_as(g_objobjects + 0x14, n); return n; }

uintptr_t object_at(int32_t i) {
    if (!g_objobjects || i < 0) return 0;
    uintptr_t chunks = 0; if (!rd_as(g_objobjects, chunks) || !chunks) return 0;
    uintptr_t chunk = 0; if (!rd_as(chunks + (i / kChunk) * 8, chunk) || !chunk) return 0;
    uintptr_t obj = 0; rd_as(chunk + (uintptr_t)(i % kChunk) * g_item_stride + g_item_obj, obj);
    return obj;
}

// fn(index, object) for every used slot in [from, n); fn returns false to stop. A chunk's item block is read with ONE
// guarded copy (was: three guarded reads per object).
template <class F> static void walk_objects(int32_t from, int32_t n, F&& fn) {
    if (!g_objobjects || n <= 0) return;
    if (from < 0) from = 0;
    uintptr_t chunks = 0; if (!rd_as(g_objobjects, chunks) || !chunks) return;
    std::vector<uint8_t> buf;
    for (int32_t c = from / kChunk; (int64_t)c * kChunk < n; ++c) {
        uintptr_t chunk = 0; if (!rd_as(chunks + (uintptr_t)c * 8, chunk) || !chunk) continue;
        const int32_t base = c * kChunk, lo = std::max(from, base), hi = (int32_t)std::min<int64_t>(n, (int64_t)base + kChunk);
        if (hi <= lo) continue;
        const size_t bytes = (size_t)(hi - lo) * g_item_stride;
        if (buf.size() < bytes) buf.resize(bytes);
        const bool bulk = mread(chunk + (uintptr_t)(lo - base) * g_item_stride, buf.data(), bytes);
        for (int32_t i = lo; i < hi; ++i) {
            uintptr_t o = 0;
            if (bulk) memcpy(&o, buf.data() + (size_t)(i - lo) * g_item_stride + g_item_obj, sizeof o);
            else rd_as(chunk + (uintptr_t)(i - base) * g_item_stride + g_item_obj, o);
            if (o && !fn(i, o)) return;
        }
    }
}

bool alive(uintptr_t obj) {
    if (!obj) return false;
    int32_t idx = -1; if (!rd_as(obj + off::Obj_Index, idx)) return false;
    return object_at(idx) == obj;
}

uintptr_t class_of(uintptr_t obj) { uintptr_t c = 0; if (obj) rd_as(obj + off::Obj_Class, c); return c; }
uintptr_t outer_of(uintptr_t obj) { uintptr_t o = 0; if (obj) rd_as(obj + off::Obj_Outer, o); return o; }
std::string name_of(uintptr_t obj) { FName n; if (!obj || !rd_as(obj + off::Obj_Name, n)) return ""; return name_str(n); }
bool is_cdo(uintptr_t obj) { uint32_t f = 0; rd_as(obj + off::Obj_Flags, f); return (f & (RF_ClassDefaultObject | RF_ArchetypeObject)) != 0; }   // CDOs + their template subobjects
uintptr_t super_of(uintptr_t s) { uintptr_t p = 0; if (s) rd_as(s + off::Struct_Super, p); return p; }

static void path_rec(uintptr_t obj, std::string& out, int depth) {
    uintptr_t outer = outer_of(obj);
    if (outer && depth < 32) {
        path_rec(outer, out, depth + 1);
        const uintptr_t oo = outer_of(outer);
        out += (class_of(outer) != g_cls_package && oo && class_of(oo) == g_cls_package) ? ":" : ".";
    }
    out += name_of(obj);
}
std::string path_of(uintptr_t obj) { std::string s; if (obj) path_rec(obj, s, 0); return s; }
std::string full_name(uintptr_t obj) { return obj ? name_of(class_of(obj)) + " " + path_of(obj) : ""; }

bool is_child_of(uintptr_t cls, uintptr_t parent) {
    for (int i = 0; cls && i < 64; ++i, cls = super_of(cls)) if (cls == parent) return true;
    return false;
}
bool is_a(uintptr_t obj, uintptr_t cls) { return obj && cls && is_child_of(class_of(obj), cls); }

// FName comparison index -> object indices (no strings: 8 bytes per object, one guarded read per object to build).
// Incremental for new slots; a full rebuild (slots get reused after GC) when a lookup misses, at most every 5 s.
// A name that is not in the name pool cannot belong to any object: that miss costs nothing.
static std::mutex g_idx_mtx;
static std::unordered_multimap<uint32_t, int32_t> g_by_name;
static int32_t g_indexed = 0;
static void index_objects(bool full) {
    const int32_t n = num_objects();
    if (full) {
        static double last = -1e9; const double t = now_ms();
        if (t - last < 5000) full = false; else last = t;
    }
    if (!full && n <= g_indexed) return;
    const double t0 = now_ms();
    if (full) { g_by_name.clear(); g_indexed = 0; }
    g_by_name.reserve((size_t)n + 1024);
    walk_objects(g_indexed, n, [](int32_t i, uintptr_t o) { FName nm; if (rd_as(o + off::Obj_Name, nm)) g_by_name.emplace(nm.index, i); return true; });
    g_indexed = n;
    ++g_perf.index_builds; g_perf.index_ms += now_ms() - t0;
}
static std::vector<uintptr_t> objects_named(const std::string& name, bool allow_full = true) {
    std::vector<uintptr_t> out;
    FName want[2]; const int nw = name_find_all(name, want);
    if (!nw) return out;
    for (int pass = 0; pass < (allow_full ? 2 : 1) && out.empty(); ++pass) {
        std::lock_guard<std::mutex> lk(g_idx_mtx);
        index_objects(pass == 1);
        for (int w = 0; w < nw; ++w) {
            auto r = g_by_name.equal_range(want[w].index);
            for (auto it = r.first; it != r.second; ++it) {
                uintptr_t o = object_at(it->second); FName nm;
                if (o && rd_as(o + off::Obj_Name, nm) && nm.index == want[w].index && nm.number == want[w].number &&
                    std::find(out.begin(), out.end(), o) == out.end()) out.push_back(o);
            }
        }
    }
    return out;
}
static void reindex_full() { std::lock_guard<std::mutex> lk(g_idx_mtx); index_objects(true); }

uintptr_t find_object(const std::string& path, bool allow_full_reindex) {
    size_t k = path.find_last_of(".:");
    const std::string leaf = k == std::string::npos ? path : path.substr(k + 1);
    for (int pass = 0; pass < (allow_full_reindex ? 2 : 1); ++pass) {
        if (pass == 1) reindex_full();
        for (uintptr_t o : objects_named(leaf, false)) if (_stricmp(path_of(o).c_str(), path.c_str()) == 0) return o;
    }
    return 0;
}

std::vector<uintptr_t> find_code(const std::string& pattern, size_t max) { init(); return scan(pattern, max); }

// ------------------------------------------------------------------ GMalloc
// Found 2026-11-09 (build 184976456, exe+0xA662FD0): the inlined FMemory pattern "if (!GMalloc) GCreateMalloc();
static uintptr_t gmalloc_global() {
    static uintptr_t g = 0; static bool tried = false;
    if (!tried) { tried = true; std::lock_guard<std::mutex> lk(g_init_mtx);
        g = vote({ "48 8B 0D ?? ?? ?? ?? 48 85 C9 75 0C E8 ?? ?? ?? ?? 48 8B 0D ?? ?? ?? ?? 48 8B 01 48 8B D3 FF 50 48" }, 3, 0, "GMalloc", 1000); }
    return g;
}
static void* realloc_seh(void* inst, void* fn, void* ptr, size_t bytes, uint32_t align) {
    using F = void* (*)(void*, void*, size_t, uint32_t);
    __try { return ((F)fn)(inst, ptr, bytes, align); } __except (EXCEPTION_EXECUTE_HANDLER) { return nullptr; }
}
void* game_realloc(void* ptr, size_t bytes, uint32_t alignment) {
    if (GetCurrentThreadId() != main_thread_id()) return nullptr;
    const uintptr_t g = gmalloc_global(); uintptr_t inst = 0, vt = 0, fn = 0;
    if (!g || !rd_as(g, inst) || !inst || !rd_as(inst, vt) || !rd_as(vt + 7 * 8, fn) || !fn) return nullptr;
    if (fn < g_text || fn >= g_text + g_text_size) return nullptr;           // must be game code
    return realloc_seh((void*)inst, (void*)fn, ptr, bytes, alignment);
}

// class / struct lookups by short name are cached (validated on every hit: still in the object array under that name)
struct TypedHit { uintptr_t obj = 0; FName name; };
static std::mutex g_typed_mtx;
static std::unordered_map<std::string, TypedHit> g_typed_cache;
static uintptr_t find_typed(const std::string& name_in, uintptr_t meta_or_0, bool want_class) {
    if (name_in.find('/') != std::string::npos) return find_object(name_in);
    const std::string ckey = (want_class ? "c:" : meta_or_0 ? "s:" : "o:") + lower_ascii(name_in);
    { std::lock_guard<std::mutex> lk(g_typed_mtx);
      auto it = g_typed_cache.find(ckey);
      if (it != g_typed_cache.end()) {
          FName nm; const uintptr_t o = it->second.obj;
          if (alive(o) && rd_as(o + off::Obj_Name, nm) && nm.index == it->second.name.index && nm.number == it->second.name.number) return o;
          g_typed_cache.erase(it);
      } }
    std::vector<std::string> cands = { name_in };
    if (name_in.size() > 2 && (name_in[0] == 'U' || name_in[0] == 'A' || name_in[0] == 'F') && isupper((unsigned char)name_in[1])) cands.push_back(name_in.substr(1));
    uintptr_t found = 0, fallback = 0;
    for (int pass = 0; pass < 2 && !found && !fallback; ++pass) {      // every candidate first, only then one full rebuild
        if (pass == 1) reindex_full();
        for (auto& n : cands) {
            for (uintptr_t o : objects_named(n, false)) {
                const uintptr_t c = class_of(o);
                const bool ok = want_class ? is_child_of(c, g_cls_class) : (meta_or_0 ? is_child_of(c, meta_or_0) : true);
                if (!ok) continue;
                const std::string p = path_of(o);
                if (p.rfind("/Script/", 0) == 0) { found = o; break; }          // native first
                if (!fallback) fallback = o;
            }
            if (found) break;
        }
    }
    if (!found) found = fallback;
    if (found) {
        TypedHit h; h.obj = found;
        if (rd_as(found + off::Obj_Name, h.name)) { std::lock_guard<std::mutex> lk(g_typed_mtx); if (g_typed_cache.size() > 20000) g_typed_cache.clear(); g_typed_cache[ckey] = h; }
    }
    return found;
}
uintptr_t find_class(const std::string& name) { return find_typed(name, 0, true); }
uintptr_t find_struct(const std::string& name) { return find_typed(name, g_cls_scriptstruct, false); }
uintptr_t cdo_of(uintptr_t cls) { uintptr_t d = 0; if (cls) rd_as(cls + off::Class_CDO, d); return d; }

std::vector<uintptr_t> find_all(uintptr_t cls, bool include_cdo, size_t max) {
    std::vector<uintptr_t> out; if (!cls) return out;
    const double t0 = now_ms();
    struct Done { double t0; ~Done() { ++g_perf.scans; g_perf.scan_ms += now_ms() - t0; } } done{ t0 };
    const int32_t n = num_objects();
    std::unordered_map<uintptr_t, bool> memo;           // class -> is a child of cls (the super chain is walked once per class)
    uintptr_t last_c = 0; bool last_yes = false;
    walk_objects(0, n, [&](int32_t, uintptr_t o) {
        const uintptr_t c = class_of(o); if (!c) return true;
        if (c != last_c) {
            auto it = memo.find(c);
            if (it == memo.end()) it = memo.emplace(c, is_child_of(c, cls)).first;
            last_c = c; last_yes = it->second;
        }
        if (!last_yes) return true;
        if (!include_cdo && is_cdo(o)) return true;
        out.push_back(o);
        return !(max && out.size() >= max);
    });
    return out;
}
uintptr_t find_first(uintptr_t cls) { auto v = find_all(cls, false, 1); return v.empty() ? 0 : v[0]; }

// ------------------------------------------------------------------ properties
static std::string ffield_class_name(uintptr_t f) {
    uintptr_t fc = 0; if (!rd_as(f + off::FField_Class, fc) || !fc) return "";
    FName n; if (!rd_as(fc, n)) return "";
    return name_str(n);
}
Prop prop_info(uintptr_t f) {
    Prop p; if (!f) return p;
    p.p = f;
    FName n; rd_as(f + off::FField_Name, n); p.name = name_str(n);
    p.type = ffield_class_name(f);
    rd_as(f + off::Prop_Offset, p.offset); rd_as(f + off::Prop_ElemSize, p.size); rd_as(f + off::Prop_ArrayDim, p.dim);
    rd_as(f + off::Prop_Flags, p.flags);
    if (p.type == "StructProperty") rd_as(f + off::Prop_Struct, p.owner_struct);
    else if (p.type == "ObjectProperty" || p.type == "ClassProperty" || p.type == "WeakObjectProperty" || p.type == "SoftObjectProperty") rd_as(f + off::Prop_ObjClass, p.owner_struct);
    else if (p.type == "ArrayProperty") rd_as(f + off::Prop_ArrayInner, p.owner_struct);
    return p;
}
std::vector<Prop> props(uintptr_t s, bool with_super) {
    std::vector<Prop> out;
    for (int depth = 0; s && depth < 64; ++depth, s = with_super ? super_of(s) : 0) {
        uintptr_t f = 0; rd_as(s + off::Struct_ChildProps, f);
        for (int i = 0; f && i < 4096; ++i) { out.push_back(prop_info(f)); rd_as(f + off::FField_Next, f); }
        if (!with_super) break;
    }
    return out;
}
Prop find_prop(uintptr_t s, const std::string& name) {
    for (int depth = 0; s && depth < 64; ++depth, s = super_of(s)) {
        uintptr_t f = 0; rd_as(s + off::Struct_ChildProps, f);
        for (int i = 0; f && i < 4096; ++i) {
            FName n; rd_as(f + off::FField_Name, n);
            if (_stricmp(name_str(n).c_str(), name.c_str()) == 0) return prop_info(f);   // FNames are case-insensitive (BL4: "player")
            rd_as(f + off::FField_Next, f);
        }
    }
    return Prop();
}
std::vector<uintptr_t> funcs(uintptr_t cls, bool with_super) {
    std::vector<uintptr_t> out;
    for (int depth = 0; cls && depth < 64; ++depth, cls = with_super ? super_of(cls) : 0) {
        uintptr_t f = 0; rd_as(cls + off::Struct_Children, f);
        for (int i = 0; f && i < 8192; ++i) { if (is_child_of(class_of(f), g_cls_function)) out.push_back(f); rd_as(f + off::Field_Next, f); }
        if (!with_super) break;
    }
    return out;
}
// (class, function name) -> UFunction, validated on every hit (still alive, same name, still owned by the class chain)
static std::mutex g_fn_mtx;
static std::map<std::pair<uintptr_t, std::string>, uintptr_t> g_fn_cache;
uintptr_t find_func(uintptr_t cls, const std::string& name) {
    if (!cls) return 0;
    const std::pair<uintptr_t, std::string> key(cls, lower_ascii(name));
    { std::lock_guard<std::mutex> lk(g_fn_mtx);
      auto it = g_fn_cache.find(key);
      if (it != g_fn_cache.end()) {
          const uintptr_t f = it->second;
          if (alive(f) && is_child_of(cls, outer_of(f)) && _stricmp(name_of(f).c_str(), name.c_str()) == 0) return f;
          g_fn_cache.erase(it);
      } }
    for (int depth = 0; cls && depth < 64; ++depth, cls = super_of(cls)) {
        uintptr_t f = 0; rd_as(cls + off::Struct_Children, f);
        for (int i = 0; f && i < 8192; ++i) {
            if (_stricmp(name_of(f).c_str(), name.c_str()) == 0 && is_child_of(class_of(f), g_cls_function)) {
                std::lock_guard<std::mutex> lk(g_fn_mtx);
                if (g_fn_cache.size() > 20000) g_fn_cache.clear();
                g_fn_cache[key] = f; return f;
            }
            rd_as(f + off::Field_Next, f);
        }
    }
    return 0;
}
Prop array_inner(const Prop& arr) { return prop_info(arr.owner_struct); }
bool map_kv(const Prop& map, Prop& key, Prop& val, int32_t& value_offset, int32_t& stride) {
    uintptr_t k = 0, v = 0; if (!rd_as(map.p + off::Prop_MapKey, k) || !rd_as(map.p + off::Prop_MapValue, v)) return false;
    key = prop_info(k); val = prop_info(v);
    int32_t layout[4] = {}; if (!mread(map.p + off::Prop_MapLayout, layout, sizeof layout)) return false;
    value_offset = layout[0]; stride = layout[3];       // FScriptMapLayout { ValueOffset; SetLayout { HashNextIdOffset; HashIndexOffset; Size } }
    return stride > 0 && stride < 0x10000;
}

bool get_bool(uintptr_t base, const Prop& p) {
    uint8_t off_ = 0, mask = 0; rd_as(p.p + off::Prop_BoolByteOffset, off_); rd_as(p.p + off::Prop_BoolFieldMask, mask);
    uint8_t b = 0; rd_as(base + p.offset + off_, b); return (b & mask) != 0;
}
bool set_bool(uintptr_t base, const Prop& p, bool v) {
    uint8_t off_ = 0, mask = 0; rd_as(p.p + off::Prop_BoolByteOffset, off_); rd_as(p.p + off::Prop_BoolFieldMask, mask);
    uint8_t b = 0; if (!rd_as(base + p.offset + off_, b)) return false;
    b = v ? (b | mask) : (b & ~mask); return mwrite(base + p.offset + off_, &b, 1);
}
double get_num(uintptr_t base, const Prop& p) {
    const auto& t = p.type;
    if (t == "BoolProperty") return get_bool(base, p) ? 1 : 0;
    if (t == "FloatProperty") return get<float>(base, p);
    if (t == "DoubleProperty") return get<double>(base, p);
    if (t == "IntProperty") return get<int32_t>(base, p);
    if (t == "Int64Property") return (double)get<int64_t>(base, p);
    if (t == "UInt32Property") return get<uint32_t>(base, p);
    if (t == "UInt64Property") return (double)get<uint64_t>(base, p);
    if (t == "UInt16Property") return get<uint16_t>(base, p);
    if (t == "Int16Property") return get<int16_t>(base, p);
    if (t == "ByteProperty" || t == "Int8Property") return get<uint8_t>(base, p);
    if (t == "EnumProperty") { if (p.size == 1) return get<uint8_t>(base, p); if (p.size == 2) return get<uint16_t>(base, p); return get<int32_t>(base, p); }
    return 0;
}
bool set_num(uintptr_t base, const Prop& p, double v) {
    const auto& t = p.type;
    if (t == "BoolProperty") return set_bool(base, p, v != 0);
    if (t == "FloatProperty") return set<float>(base, p, (float)v);
    if (t == "DoubleProperty") return set<double>(base, p, v);
    if (t == "IntProperty") return set<int32_t>(base, p, (int32_t)llround(v));
    if (t == "Int64Property") return set<int64_t>(base, p, (int64_t)llround(v));
    if (t == "UInt32Property") return set<uint32_t>(base, p, (uint32_t)llround(v));
    if (t == "UInt64Property") return set<uint64_t>(base, p, (uint64_t)llround(v));
    if (t == "UInt16Property") return set<uint16_t>(base, p, (uint16_t)llround(v));
    if (t == "Int16Property") return set<int16_t>(base, p, (int16_t)llround(v));
    if (t == "ByteProperty" || t == "Int8Property") return set<uint8_t>(base, p, (uint8_t)llround(v));
    if (t == "EnumProperty") { if (p.size == 1) return set<uint8_t>(base, p, (uint8_t)llround(v)); if (p.size == 2) return set<uint16_t>(base, p, (uint16_t)llround(v)); return set<int32_t>(base, p, (int32_t)llround(v)); }
    return false;
}

bool resolve(uintptr_t base, uintptr_t s, const std::string& dotted, uintptr_t& out_base, Prop& leaf) {
    size_t a = 0;
    while (true) {
        size_t b = dotted.find('.', a);
        const std::string part = dotted.substr(a, b == std::string::npos ? std::string::npos : b - a);
        Prop p = find_prop(s, part); if (!p) return false;
        if (b == std::string::npos) { out_base = base; leaf = p; return true; }
        if (p.type == "GbxInlineStructProperty") { uintptr_t inst = 0, st = 0; if (!inline_target(base + p.offset, inst, st)) return false; base = inst; s = st; a = b + 1; continue; }
        if (p.type != "StructProperty") return false;
        base += p.offset; s = p.owner_struct; a = b + 1;
    }
}

TArray<uint8_t> array_of(uintptr_t base, const Prop& arr) { TArray<uint8_t> a; if (arr) mread(base + arr.offset, &a, sizeof a); return a; }

std::vector<std::pair<uintptr_t, uintptr_t>> map_entries(uintptr_t base, const Prop& map) {
    std::vector<std::pair<uintptr_t, uintptr_t>> out;
    Prop k, v; int32_t voff = 0, stride = 0;
    if (!map_kv(map, k, v, voff, stride)) return out;
    // TSparseArray: TArray Data (+0x00), TBitArray AllocationFlags (+0x10: uint32 Inline[4], uint32* Secondary, int NumBits, int MaxBits)
    const uintptr_t m = base + map.offset;
    TArray<uint8_t> data; if (!mread(m, &data, sizeof data) || !data.data || data.num <= 0 || data.num > 1000000) return out;
    uint32_t inl[4] = {}; uintptr_t secondary = 0; int32_t numbits = 0, maxbits = 0;
    mread(m + 0x10, inl, 16); rd_as(m + 0x20, secondary); rd_as(m + 0x28, numbits); rd_as(m + 0x2C, maxbits);
    std::vector<uint32_t> bits((data.num + 31) / 32);
    if (secondary) mread(secondary, bits.data(), bits.size() * 4);
    else for (size_t i = 0; i < bits.size() && i < 4; ++i) bits[i] = inl[i];
    for (int32_t i = 0; i < data.num; ++i) {
        if (!(bits[i / 32] & (1u << (i % 32)))) continue;
        const uintptr_t e = (uintptr_t)data.data + (uintptr_t)i * stride;
        out.push_back({ e, e + voff });
    }
    return out;
}

// ------------------------------------------------------------------ strings
std::string fstring(uintptr_t a) {
    TArray<wchar_t> s; if (!a || !mread(a, &s, sizeof s) || !s.data || s.num <= 0 || s.num > 1 << 20) return "";
    std::wstring w(s.num, L'\0'); if (!mread((uintptr_t)s.data, w.data(), s.num * 2)) return "";
    while (!w.empty() && w.back() == 0) w.pop_back();
    int u = WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), nullptr, 0, nullptr, nullptr);
    std::string out(u, '\0'); WideCharToMultiByte(CP_UTF8, 0, w.data(), (int)w.size(), out.data(), u, nullptr, nullptr);
    return out;
}
// FText -> utf8 through the game (KismetTextLibrary.Conv_TextToString). The returned FString is engine memory we cannot
// free (no GMalloc), so results are cached by the FText's own bytes (text data pointer + flags) for 30 s: a readout
// that shows the same text every second converts it twice a minute instead of sixty times.
struct TextHit { std::string s; double at = 0; };
static std::mutex g_text_mtx;
static std::map<std::string, TextHit> g_text_cache;
std::string ftext(uintptr_t a) {
    static uintptr_t lib = 0, fn = 0; static size_t in_size = 0; static double next_lookup = 0;
    if (!a) return "";
    if (!fn || !lib || !in_size) {
        const double t = now_ms(); if (t < next_lookup) return ""; next_lookup = t + 5000;
        lib = find_object("/Script/Engine.Default__KismetTextLibrary"); fn = find_object("/Script/Engine.KismetTextLibrary:Conv_TextToString");
        if (lib && fn) { Params p0(fn); Prop in0 = p0 ? p0.param("InText") : Prop(); in_size = in0 ? std::min<size_t>((size_t)std::max(in0.size, 0), 0x18) : 0; }
        if (!lib || !fn || !in_size) return "";
    }
    uint8_t raw[0x18] = {}; if (!mread(a, raw, in_size)) return "";
    const std::string key((const char*)raw, in_size);
    const double now = now_ms();
    { std::lock_guard<std::mutex> lk(g_text_mtx);
      auto it = g_text_cache.find(key);
      if (it != g_text_cache.end() && now - it->second.at < 30000) return it->second.s; }
    Params p(fn); if (!p) return "";
    Prop in = p.param("InText"); if (!in) return "";
    memcpy(p.at(in), raw, std::min<size_t>(in_size, (size_t)std::max(in.size, 0)));
    if (!call(lib, p)) return "";
    uint8_t* r = p.ret_ptr(); if (!r) return "";
    std::string out = fstring((uintptr_t)r);   // (the returned FString buffer is left to the game's allocator)
    { std::lock_guard<std::mutex> lk(g_text_mtx);
      if (g_text_cache.size() > 5000) g_text_cache.clear();
      g_text_cache[key] = TextHit{ out, now }; }
    return out;
}

// ------------------------------------------------------------------ calling
Params::Params(uintptr_t func) : fn(func) {
    uint16_t size = 0; if (!func || !rd_as(func + off::Func_ParmsSize, size)) { fn = 0; return; }
    buf.assign(size + 16, 0);
}
Prop Params::param(const std::string& name) const {
    uintptr_t f = 0; rd_as(fn + off::Struct_ChildProps, f);
    for (int i = 0; f && i < 64; ++i) {
        FName n; rd_as(f + off::FField_Name, n);
        if (_stricmp(name_str(n).c_str(), name.c_str()) == 0) return prop_info(f);   // FNames are case-insensitive (same as find_prop)
        rd_as(f + off::FField_Next, f);
    }
    return Prop();
}
bool Params::put_raw(const std::string& name, const void* src, size_t n) {
    Prop p = param(name); if (!p) return false; memcpy(at(p), src, std::min<size_t>(n, p.size)); return true;
}
bool Params::put_bool(const std::string& name, bool v) {
    Prop p = param(name); if (!p) return false;
    uint8_t off_ = 0, mask = 0; rd_as(p.p + off::Prop_BoolByteOffset, off_); rd_as(p.p + off::Prop_BoolFieldMask, mask);
    uint8_t& b = buf[p.offset + off_]; b = v ? (b | mask) : (b & ~mask); return true;
}

typedef void (*ProcessEventFn)(uintptr_t, uintptr_t, void*);
static int seh_pe(uintptr_t pe, uintptr_t obj, uintptr_t fn, void* params, DWORD* code) {
    int r = 0;
    __try { ((ProcessEventFn)pe)(obj, fn, params); r = 1; }
    __except ((*code = GetExceptionCode()), EXCEPTION_EXECUTE_HANDLER) { r = 0; }
    return r;
}

std::string func_path(uintptr_t fn) { return path_of(fn); }

void journal_init(const std::string& dir) {
    g_journal_dir = dir;
    std::ifstream bf(dir + "\\blocked.txt"); std::string line;
    while (std::getline(bf, line)) { while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back(); if (!line.empty()) g_blocked_paths.push_back(line); }
    std::ifstream inf(dir + "\\inflight.txt");
    if (inf && std::getline(inf, line) && !line.empty()) {
        const std::string fnp = line.substr(0, line.find('\t'));
        log("[ue] the game closed during %s last time - that function is now blocked", fnp.c_str());
        g_blocked_paths.push_back(fnp);
        std::ofstream(dir + "\\blocked.txt", std::ios::app) << fnp << "\n";
        inf.close(); DeleteFileA((dir + "\\inflight.txt").c_str());
    }
}
void journal_scope(bool on) { t_journal = on; }
// Verdict caches are keyed by the function's address. A Blueprint function can be unloaded and its address reused by
// another one, so every entry remembers which function (name + outer) it was made for.
struct FnId { FName name; uintptr_t outer = 0; };
static FnId fn_id(uintptr_t fn) { FnId i; rd_as(fn + off::Obj_Name, i.name); rd_as(fn + off::Obj_Outer, i.outer); return i; }
static bool same_fn(const FnId& a, const FnId& b) { return a.name.index == b.name.index && a.name.number == b.name.number && a.outer == b.outer; }
static std::mutex g_verdict_mtx;
static std::unordered_map<uintptr_t, std::pair<FnId, bool>> g_blocked_fn;
bool blocked(uintptr_t fn) {
    const FnId id = fn_id(fn);
    { std::lock_guard<std::mutex> lk(g_verdict_mtx);
      auto it = g_blocked_fn.find(fn); if (it != g_blocked_fn.end() && same_fn(it->second.first, id)) return it->second.second; }
    const std::string p = func_path(fn); bool b = false;
    for (auto& x : g_blocked_paths) if (x == p) { b = true; break; }
    std::lock_guard<std::mutex> lk(g_verdict_mtx);
    if (g_blocked_fn.size() > 50000) g_blocked_fn.clear();
    g_blocked_fn[fn] = { id, b }; return b;
}

// A function that returns / fills a STRUCT by value assigns into our zero-initialised buffer. Zero bytes are a valid empty
// FString / TArray / FName / pointer, but not a valid FText / TMap / TSet / delegate: the game's operator= then releases
// references that never existed (corrupted refcounts -> crash at game exit, 2026-11-09). Such calls are refused.
// The same goes for a TMap / TSet / delegate that is itself the return value or an out parameter. (A plain FText
// return is allowed: call_json reads those, and FText's reference is a null-safe pointer.)
static std::map<uintptr_t, std::pair<FnId, std::string>> g_unsafe_cache;
static bool unsafe_type(const std::string& t, bool with_text) {
    return (with_text && t == "TextProperty") || t == "MapProperty" || t == "SetProperty" || t == "DelegateProperty" ||
           t == "MulticastInlineDelegateProperty" || t == "MulticastSparseDelegateProperty" || t == "MulticastDelegateProperty";
}
static std::string unsafe_member(uintptr_t ustruct, int depth = 0) {
    if (!ustruct || depth > 6) return "";
    for (auto& m : props(ustruct, true)) {
        if (unsafe_type(m.type, true)) return name_of(ustruct) + "." + m.name + " (" + m.type + ")";
        if (m.type == "StructProperty") { std::string u = unsafe_member(m.owner_struct, depth + 1); if (!u.empty()) return u; }
    }
    return "";
}
static std::string unsafe_struct_out(uintptr_t fn) {
    const FnId id = fn_id(fn);
    { std::lock_guard<std::mutex> lk(g_verdict_mtx);
      auto it = g_unsafe_cache.find(fn); if (it != g_unsafe_cache.end() && same_fn(it->second.first, id)) return it->second.second; }
    std::string why;
    for (auto& pp : props(fn, false)) {
        if (!(pp.flags & (CPF_ReturnParm | CPF_OutParm))) continue;
        if (!(pp.flags & CPF_ReturnParm) && (pp.flags & 0x2)) continue;            // const& input (CPF_ConstParm) - we pass a valid copy
        if (unsafe_type(pp.type, false)) { why = pp.name + " (" + pp.type + ")"; break; }
        if (pp.type != "StructProperty") continue;
        why = unsafe_member(pp.owner_struct); if (!why.empty()) { why = pp.name + " -> " + why; break; }
    }
    std::lock_guard<std::mutex> lk(g_verdict_mtx);
    if (g_unsafe_cache.size() > 50000) g_unsafe_cache.clear();
    g_unsafe_cache[fn] = { id, why }; return why;
}

bool call(uintptr_t obj, Params& p, std::string* err) {
    auto E = [&](const std::string& m) { if (err) *err = m; return false; };
    if (!g_ready) return E("engine core not ready");
    if (g_game_exiting) return E("the game is closing");
    if (!obj || !p.fn) return E("no object / function");
    if (GetCurrentThreadId() != main_thread_id()) return E("not on the game thread");
    if (!alive(obj)) return E("the object is gone (not in the object array any more)");
    if (!alive(p.fn)) return E("the function is gone (unloaded)");
    if (blocked(p.fn)) return E("blocked: " + func_path(p.fn) + " crashed the game before");
    { const std::string why = unsafe_struct_out(p.fn);
      if (!why.empty()) { log("[ue] refused %s: returns a struct the engine cannot build safely (%s)", func_path(p.fn).c_str(), why.c_str());
                          return E("refused: " + func_path(p.fn) + " returns " + why); } }
    uintptr_t pe = g_process_event;
    if (!pe && g_pe_slot >= 0) { uintptr_t vt = 0; if (rd_as(obj, vt)) rd_as(vt + g_pe_slot * 8, pe); }
    if (!pe) return E("ProcessEvent not found");
    const std::string path = (g_journal_dir.empty() || !t_journal) ? "" : func_path(p.fn);
    if (!path.empty()) { std::ofstream f(g_journal_dir + "\\inflight.txt", std::ios::trunc); f << path << "\t" << GetTickCount64() << "\n"; }
    DWORD code = 0;
    const int r = seh_pe(pe, obj, p.fn, p.buf.data(), &code);
    if (!path.empty()) DeleteFileA((g_journal_dir + "\\inflight.txt").c_str());
    if (!r && code == EXCEPTION_STACK_OVERFLOW) _resetstkoflw();   // put the stack guard page back, or the next overflow kills the process
    if (!r) { log("[ue] %s raised 0x%08X (caught)", func_path(p.fn).c_str(), code); return E("the game function raised " + hexstr(code) + " (caught)"); }
    return true;
}
bool call(uintptr_t obj, const std::string& func, Params& p, std::string* err) {
    if (!p.fn) { p = Params(find_func(class_of(obj), func)); if (!p) { if (err) *err = "no function " + func + " on " + name_of(class_of(obj)); return false; } }
    return call(obj, p, err);
}
Params params_for(uintptr_t obj, const std::string& func) { return Params(find_func(class_of(obj), func)); }

// ------------------------------------------------------------------ start-up
static bool validate_objects(uintptr_t objobjects) {
    int32_t maxe = 0, nume = 0, numchunks = 0; uintptr_t chunks = 0;
    if (!rd_as(objobjects, chunks) || !rd_as(objobjects + 0x10, maxe) || !rd_as(objobjects + 0x14, nume) || !rd_as(objobjects + 0x1C, numchunks)) return false;
    if (!chunks || nume < 1000 || nume > maxe || maxe > 50000000 || numchunks < 1 || numchunks > 1000) return false;
    // item layout: FUObjectItem 0x18; UE 5.7 keeps FlagsAndRefCount first and the object at +8. Objects know their own index.
    for (int objoff : { 8, 0 }) for (int stride : { 0x18, 0x10, 0x20 }) {
        g_objobjects = objobjects; g_item_stride = stride; g_item_obj = objoff;
        int good = 0;
        for (int i = 1; i <= 64; ++i) { uintptr_t o = object_at(i * 7); int32_t idx = -1; if (o && rd_as(o + off::Obj_Index, idx) && idx == i * 7) ++good; }
        if (good >= 48) return true;
    }
    g_objobjects = 0; return false;
}

// Code-pattern scans cannot change for a running exe: each one runs once and its result is kept, so a retry only repeats
// the structure checks / .data walks (needed when the trainer attached before the engine filled its tables).
static bool init_locked() {
    if (!text_section()) { g_found["error"] = "game module not found"; return false; }
    g_found.erase("error");
    // GUObjectArray.ObjObjects: "mov rax,[ObjObjects.Objects]; mov rcx,[rax+rcx*8]; lea rax/r8,[rcx+rdx*8]" (hundreds of sites)
    static bool oo_voted = false; static uintptr_t oo_vote = 0, oo_ok = 0;
    if (!oo_voted) { oo_voted = true; oo_vote = vote({ "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 48 8D 04 D1", "48 8B 05 ?? ?? ?? ?? 48 8B 0C C8 4C 8D 04 D1" }, 3, 0, "GUObjectArray.ObjObjects"); }
    uintptr_t oo = oo_ok ? oo_ok : oo_vote;      // an array that validated before is tried first (no .data walk)
    if (!oo || !validate_objects(oo)) {
        // Fallback (Borderlands 4: the access patterns are not in its .text): walk .data for an FChunkedFixedUObjectArray
        // {Objects**, PreAllocated*, Max, Num, MaxChunks, NumChunks} whose items point at objects knowing their own index.
        oo = 0;
        for (size_t o = 0; g_data && o + 0x20 <= g_data_size && !oo; o += 8) {
            uintptr_t chunks = 0; int32_t v[4] = {};
            if (!rd_as(g_data + o, chunks) || chunks < 0x10000 || !mread(g_data + o + 0x10, v, sizeof v)) continue;
            if (v[1] < 1000 || v[1] > v[0] || v[0] > 50000000 || v[3] < 1 || v[3] > 1000 || v[2] < v[3]) continue;
            if (validate_objects(g_data + o)) oo = g_data + o;
        }
        g_found["GUObjectArray.ObjObjects.scan"] = oo ? hexstr(oo - g_base) : "-";
        if (!oo) { g_found["error"] = "object array not found / did not validate"; return false; }
    }
    oo_ok = oo;
    // FNamePool: lazy-init sites "lea rcx,[NamePool]; call Init; mov r8,rax; mov byte [inited],1" (+ variants)
    static bool np_voted = false; static uintptr_t np_vote = 0, np_ok = 0;
    if (!np_voted) { np_voted = true;
        np_vote = vote({ "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 4C 8B C0 C6 05 ?? ?? ?? ?? 01",
                         "4C 8D 05 ?? ?? ?? ?? EB ?? 48 8D 0D ?? ?? ?? ?? E8",
                         "48 8D 0D ?? ?? ?? ?? E8 ?? ?? ?? ?? 48 8B D0 C6 05 ?? ?? ?? ?? 01" }, 3, 0, "FNamePool"); }
    g_namepool = np_ok ? np_ok : np_vote;
    if (!g_namepool || name_of(object_at(0)) != "/Script/CoreUObject") {
        // Fallback: walk .data for FNameEntryAllocator {lock, CurrentBlock, Cursor, Blocks[]} whose first entry is "None"
        g_namepool = 0; g_name_cache.clear();
        for (size_t o = 0; g_data && o + 0x18 <= g_data_size; o += 8) {
            uint32_t cb = 0, cur = 0; uintptr_t blk = 0; char nm[6] = {};
            if (!rd_as(g_data + o + 8, cb) || cb > 8192 || !rd_as(g_data + o + 0xC, cur) || cur > 0x40000) continue;
            if (!rd_as(g_data + o + 0x10, blk) || blk < 0x10000 || !mread(blk, nm, 6)) continue;
            if (memcmp(nm + 2, "None", 4) != 0 || ((uint8_t)nm[0] | ((uint8_t)nm[1] << 8)) >> 6 != 4) continue;
            g_namepool = g_data + o;
            if (name_of(object_at(0)) == "/Script/CoreUObject") break;
            g_namepool = 0; g_name_cache.clear();
        }
        g_found["FNamePool.scan"] = g_namepool ? hexstr(g_namepool - g_base) : "-";
    }
    // name check: object #0 is a package called "/Script/CoreUObject"
    if (!g_namepool || name_of(object_at(0)) != "/Script/CoreUObject") { g_found["error"] = "name pool did not validate (object 0 = '" + name_of(object_at(0)) + "')"; g_namepool = 0; return false; }
    np_ok = g_namepool;
    // core classes
    for (int32_t i = 0; i < 4096 && !(g_cls_package && g_cls_class && g_cls_function && g_cls_scriptstruct); ++i) {
        uintptr_t o = object_at(i); if (!o || name_of(outer_of(o)) != "/Script/CoreUObject") continue;
        const std::string n = name_of(o);
        if (n == "Package") g_cls_package = o; else if (n == "Class") g_cls_class = o;
        else if (n == "Function") g_cls_function = o; else if (n == "ScriptStruct") g_cls_scriptstruct = o;
    }
    if (!g_cls_package || !g_cls_class || !g_cls_function) { g_found["error"] = "core classes not found"; return false; }
    // ProcessEvent: its own prologue (TQ2 build 145062: exe+0x151B0C0, UObject vtable slot 76, unique); the vtable slot is
    // learned now as the update fallback. If the pattern breaks after a patch, slot 76 of UObject's CDO is tried.
    static bool pe_scanned = false; static std::vector<uintptr_t> pe;
    if (!pe_scanned) { pe_scanned = true; pe = scan("40 55 56 57 41 54 41 55 41 56 41 57 48 81 EC 60 01 00 00 48 8D 6C 24 30", 4); }
    if (pe.size() == 1) g_process_event = pe[0];
    uintptr_t probe = 0;
    for (int32_t i = 0; i < 64 && !probe; ++i) { uintptr_t o = object_at(i); if (o && name_of(o) == "Object" && class_of(o) == g_cls_class) rd_as(o + off::Class_CDO, probe); }
    if (!probe) probe = object_at(0);
    {   uintptr_t vt = 0;
        if (rd_as(probe, vt) && g_process_event) for (int s = 0; s < 200; ++s) { uintptr_t e = 0; if (rd_as(vt + s * 8, e) && e == g_process_event) { g_pe_slot = s; break; } }
        if (!g_process_event && rd_as(probe, vt)) {   // fallback: slot 76 if it starts like ProcessEvent (push rbp/rsi/rdi/r12-r15; sub rsp)
            uintptr_t e = 0; uint8_t b[16] = {};
            if (rd_as(vt + 76 * 8, e) && mread(e, b, sizeof b) && b[0] == 0x40 && b[1] == 0x55 && b[12] == 0x48 && b[13] == 0x81 && b[14] == 0xEC) { g_process_event = e; g_pe_slot = 76; }
        }
        // fallback 2 (Borderlands 4, clang build: ProcessEvent = exe+0xD5A18, slot 73): the UObject virtual whose first
        // 0x80 bytes test Function->FunctionFlags FUNC_Native - "test byte ptr [rdx+0xB1], 4" (F6 82 B1 00 00 00 04).
        if (!g_process_event && rd_as(probe, vt)) {
            static const uint8_t sig[] = { 0xF6, 0x82, 0xB1, 0x00, 0x00, 0x00, 0x04 };
            for (int s = 40; s < 120 && !g_process_event; ++s) {
                uintptr_t e = 0; uint8_t b[0x80] = {};
                if (!rd_as(vt + s * 8, e) || e < g_text || e >= g_text + g_text_size || !mread(e, b, sizeof b)) continue;
                for (size_t k = 0; k + sizeof sig <= sizeof b; ++k)
                    if (memcmp(b + k, sig, sizeof sig) == 0) { g_process_event = e; g_pe_slot = s; break; }
            }
        }
    }
    g_found["ProcessEvent"] = { {"rva", g_process_event ? hexstr(g_process_event - g_base) : "-"}, {"vtable_slot", g_pe_slot}, {"pattern_hits", pe.size()} };
    if (!g_process_event && g_pe_slot < 0) { g_found["error"] = "ProcessEvent not found"; return false; }
    g_found["objects"] = num_objects(); g_found["item_stride"] = hexstr(g_item_stride);
    g_ready = true;
    log("[ue] engine core ready: %d objects, names ok, ProcessEvent %s (slot %d)", num_objects(),
        g_process_event ? hexstr(g_process_event - g_base).c_str() : "via vtable", g_pe_slot);
    return true;
}
bool init() {
    if (g_ready) return true;
    std::lock_guard<std::mutex> lk(g_init_mtx);
    if (g_ready) return true;
    static double next_try = 0; static int fails = 0;
    const double t = now_ms();
    if (t < next_try) return false;                    // failed a moment ago: back off (2, 4, 8, 15 s) instead of rescanning
    if (init_locked()) { g_found.erase("retry_in_ms"); return true; }
    ++fails;
    const double wait = std::min(15000.0, 2000.0 * (double)(1 << std::min(fails - 1, 3)));
    next_try = t + wait; g_found["retry_in_ms"] = (int)wait; g_found["failed_attempts"] = fails;
    return false;
}
bool ready() { return g_ready; }
json status() { json j; { std::lock_guard<std::mutex> lk(g_init_mtx); j = g_found; } j["ready"] = g_ready.load(); return j; }

static json prop_json(const Prop& p) {
    json j = { {"name", p.name}, {"type", p.type}, {"offset", hexstr(p.offset)}, {"size", p.size} };
    if (p.dim != 1) j["dim"] = p.dim;
    if (p.owner_struct) j["of"] = name_of(p.owner_struct);
    if (p.flags & CPF_ReturnParm) j["ret"] = true; else if (p.flags & CPF_OutParm) j["out"] = true;
    return j;
}
static uintptr_t obj_arg(const json& a) {
    if (a.contains("addr")) { uintptr_t v = 0; std::string e; eval_addr(a["addr"].get<std::string>(), v, &e); return v; }
    if (a.contains("path")) return find_object(a["path"].get<std::string>());
    if (a.contains("class")) { uintptr_t c = find_class(a["class"].get<std::string>()); return a.value("cdo", false) ? cdo_of(c) : find_first(c); }
    return 0;
}

// Loads with KismetSystemLibrary::LoadAsset_Blocking and reads everything in the same game-thread call (soft tables
// nobody references are garbage collected right after). Values are summarised (numbers, names, enums, bools, soft
// paths, object names, texts, array sizes, one level of nested structs).
// Gearbox "GbxHasStructType" objects (defs, value resolvers, any GbxInlineStruct payload) report their UScriptStruct
// through a tiny virtual: [jmp rel32 ->] (sub rsp,28 ;) mov rax,[rip+disp] -> the cached struct. Tried on slots 1, 0, 2.
uintptr_t typed_struct_of(uintptr_t inst) {
    uintptr_t vt = 0; if (!inst || !rd_as(inst, vt) || vt < 0x10000) return 0;
    for (int slot : { 1, 0, 2 }) {
        uintptr_t fn = 0; if (!rd_as(vt + slot * 8, fn) || fn < 0x10000) continue;
        uint8_t b[16] = {}; if (!mread(fn, b, sizeof b)) continue;
        if (b[0] == 0xE9) { int32_t rel = 0; memcpy(&rel, b + 1, 4); fn = fn + 5 + rel; if (!mread(fn, b, sizeof b)) continue; }
        uintptr_t stat = 0;
        if (b[0] == 0x48 && b[1] == 0x83 && b[2] == 0xEC && b[4] == 0x48 && b[5] == 0x8B && b[6] == 0x05) { int32_t d = 0; memcpy(&d, b + 7, 4); stat = fn + 11 + d; }
        else if (b[0] == 0x48 && b[1] == 0x8B && b[2] == 0x05) { int32_t d = 0; memcpy(&d, b + 3, 4); stat = fn + 7 + d; }
        if (!stat) continue;
        uintptr_t st = 0; if (!rd_as(stat, st) || !st || !alive(st)) continue;
        if (name_of(class_of(st)) == "ScriptStruct") return st;
    }
    return 0;
}
// GbxInlineStruct (24 bytes): one of its words points at the payload instance (a GbxHasStructType). -> instance + its struct
bool inline_target(uintptr_t field, uintptr_t& inst, uintptr_t& st) {
    inst = st = 0; uintptr_t q[3] = {}; if (!mread(field, q, sizeof q)) return false;
    for (uintptr_t p : q) if (p > 0x10000 && p < 0x7FFFFFFFFFFF) { if (uintptr_t t = typed_struct_of(p)) { inst = p; st = t; return true; } }
    return false;
}
static thread_local int t_maxd = 4;   // how deep struct reads go (UE.STRUCT / DEF_GET {depth})
static json prop_value(uintptr_t base, const Prop& p, int depth);
static json struct_value(uintptr_t base, uintptr_t st, int depth) {
    json o = json::object();
    for (auto& c : props(st, true)) o[c.name] = prop_value(base, c, depth + 1);
    return o;
}
static json prop_value(uintptr_t base, const Prop& p, int depth) {
    const std::string& t = p.type; const uintptr_t a = base + p.offset;
    if (t == "BoolProperty") return get_bool(base, p);
    if (t == "NameProperty") { FName n{}; mread(a, &n, sizeof n); return name_str(n); }
    if (t == "StrProperty") return fstring(a);
    if (t == "TextProperty") return ftext(a);
    if (t == "GbxDefPtrProperty") { const std::string d = def_text(a); return d.empty() ? json(nullptr) : json(d); }   // which def it points at
    if (t == "GameDataHandleProperty") { FName n{}; mread(a, &n, 8); const std::string d = name_str(n); return (d.empty() || d == "None") ? json(nullptr) : json("gdh:" + d); }   // {name, category}: the name is enough to find it
    if (t == "SoftObjectProperty" || t == "SoftClassProperty") { FName pk{}, as{}; mread(a + 8, &pk, 8); mread(a + 0x10, &as, 8);
        const std::string s1 = name_str(pk); return s1 == "None" ? json("") : json(s1 + "." + name_str(as)); }
    if (t == "ObjectProperty" || t == "ClassProperty") { uintptr_t o = 0; rd_as(a, o); return o ? json(path_of(o)) : json(nullptr); }
    if (t == "ArrayProperty") {   // elements (first 32) when they are readable values / structs; else just the count
        int32_t n = 0; rd_as(a + 8, n); Prop in = array_inner(p);
        if (n <= 0 || !in || depth >= t_maxd || n > 4096) return json{ {"array", n} };
        uintptr_t data = 0; rd_as(a, data); if (!data) return json{ {"array", n} };
        json out = json::array(); Prop e = in; e.offset = 0;
        for (int32_t i = 0; i < n && i < 32; ++i) out.push_back(prop_value(data + (uintptr_t)i * in.size, e, depth + 1));
        if (n > 32) out.push_back(json{ {"more", n - 32} });
        return out;
    }
    if (t == "MapProperty") {   // {key: value} (first 32)
        int32_t n = 0; rd_as(a + 8, n); Prop k, v; int32_t vo = 0, st = 0;
        if (n <= 0 || depth >= t_maxd || !map_kv(p, k, v, vo, st)) return json{ {"map", n} };
        json out = json::object(); int c = 0; Prop k0 = k, v0 = v; k0.offset = 0; v0.offset = 0;
        for (auto& [ka, va] : map_entries(base, p)) { if (++c > 32) break;
            json kj = prop_value(ka, k0, depth + 1); std::string ks = kj.is_string() ? kj.get<std::string>() : kj.dump();
            out[ks] = prop_value(va, v0, depth + 1); }
        return out;
    }
    if (t == "SetProperty") { int32_t n = 0; rd_as(a + 8, n); return json{ {"set", n} }; }
    if (t == "StructProperty") { if (depth >= t_maxd || !p.owner_struct) return json(name_of(p.owner_struct)); return struct_value(a, p.owner_struct, depth); }
    if (t == "GbxInlineStructProperty") { uintptr_t inst = 0, st = 0; if (!inline_target(a, inst, st)) return json(nullptr);
        if (depth >= t_maxd + 1) return json(name_of(st)); json o = struct_value(inst, st, depth); o["@type"] = name_of(st); return o; }   // polymorphic value: its kind + fields
    return get_num(base, p);
}
uintptr_t load_asset(const std::string& path) {
    const size_t dot = path.rfind('.'); if (dot == std::string::npos) return 0;
    if (uintptr_t o = find_object(path, false)) return o;
    FName pk{}, as{}; if (!name_find(path.substr(0, dot), pk) || !name_find(path.substr(dot + 1), as)) return 0;
    uintptr_t k = cdo_of(find_class("KismetSystemLibrary")); Params p = params_for(k, "LoadAsset_Blocking"); if (!p) return 0;
    Prop sp = p.param("Asset"); if (!sp || sp.size < 0x18) return 0;      // TSoftObjectPtr: package name at +8, asset name at +0x10
    memcpy(p.at(sp) + 8, &pk, 8); memcpy(p.at(sp) + 0x10, &as, 8); if (!call(k, p)) return 0;
    return p.ret<uintptr_t>();
}

// first `max` calls with object + parameter values (inputs and outputs as they are when the call starts).
namespace trace {
static ProcessEventFn o_pe = nullptr;
static bool g_hooked = false;
static std::atomic<bool> g_on{ false };
static std::mutex g_mx;
static std::vector<std::string> g_words;           // lowercase, any match
static std::vector<std::string> g_skip;            // lowercase, excluded
static std::unordered_map<uintptr_t, int> g_match;  // fn -> 0 no / 1 yes
static std::unordered_map<uintptr_t, std::string> g_names;
static std::map<std::string, uint64_t> g_counts;
static json g_rows = json::array();
static size_t g_max = 60; static bool g_params = true; static ULONGLONG g_t0 = 0;
static thread_local int t_depth = 0;
static std::string lower(std::string s) { for (auto& c : s) c = (char)tolower((unsigned char)c); return s; }
static void on_call(uintptr_t obj, uintptr_t fn, void* params) {
    std::lock_guard<std::mutex> lk(g_mx);
    auto it = g_match.find(fn); int m;
    if (it == g_match.end()) {
        const std::string pth = path_of(fn); const std::string lp = lower(pth); m = g_words.empty() ? 1 : 0;
        for (auto& w : g_words) if (lp.find(w) != std::string::npos) { m = 1; break; }
        for (auto& w : g_skip) if (lp.find(w) != std::string::npos) { m = 0; break; }
        g_match[fn] = m; if (m) g_names[fn] = pth;
    } else m = it->second;
    if (!m) return;
    const std::string& nm = g_names[fn];
    if (++g_counts[nm] > 3 && g_rows.size() >= g_max) return;
    if (g_rows.size() >= g_max) return;
    json r = { {"t", (double)(GetTickCount64() - g_t0) / 1000.0}, {"fn", nm}, {"on", full_name(obj)} };
    if (g_params && params) {
        json ps = json::object();
        for (auto& pp : props(fn, false)) {
            if ((pp.flags & CPF_ReturnParm) || ((pp.flags & CPF_OutParm) && !(pp.flags & 0x2))) continue;   // not filled yet: garbage
            ps[pp.name] = prop_value((uintptr_t)params, pp, 0);
        }
        r["params"] = ps;
    }
    g_rows.push_back(r);
}
static void hk_pe(uintptr_t obj, uintptr_t fn, void* params) {
    if (g_on.load(std::memory_order_relaxed) && t_depth == 0 && obj && fn) {
        ++t_depth;
        try { on_call(obj, fn, params); } catch (...) {}      // we are inside the game's ProcessEvent: nothing may unwind into it
        --t_depth;
    }
    o_pe(obj, fn, params);
}
static json cmd(const json& a) {
    const std::string op = a.value("op", "start");
    if (op == "start") {
        if (!g_hooked) {
            if (!g_process_event) return fail("NO_PE", "ProcessEvent address unknown");
            if (MH_CreateHook((void*)g_process_event, (void*)&hk_pe, (void**)&o_pe) != MH_OK || MH_EnableHook((void*)g_process_event) != MH_OK)
                return fail("HOOK", "could not hook ProcessEvent");
            g_hooked = true;
        }
        std::lock_guard<std::mutex> lk(g_mx);
        g_words.clear(); g_skip.clear(); g_match.clear(); g_names.clear(); g_counts.clear(); g_rows = json::array();
        for (auto& w : a.value("words", json::array())) g_words.push_back(lower(w.get<std::string>()));
        for (auto& w : a.value("skip", json::array())) g_skip.push_back(lower(w.get<std::string>()));
        g_max = a.value("max", 60); g_params = a.value("params", true); g_t0 = GetTickCount64();
        g_on = true; return ok({ {"tracing", true}, {"words", g_words.size()} });
    }
    if (op == "stop") g_on = false;
    std::lock_guard<std::mutex> lk(g_mx);
    std::vector<std::pair<uint64_t, std::string>> cs; for (auto& kv : g_counts) cs.push_back({ kv.second, kv.first });
    std::sort(cs.rbegin(), cs.rend()); json counts = json::array();
    for (size_t i = 0; i < cs.size() && i < (size_t)a.value("top", 80); ++i) counts.push_back({ {"n", cs[i].first}, {"fn", cs[i].second} });
    return ok({ {"tracing", g_on.load()}, {"counts", counts}, {"rows", g_rows} });
}
} // namespace trace

// ------------------------------------------------------------------ UMG: the game's own screens (menus, HUD, pages)
// UserWidget.WidgetTree.RootWidget, then PanelWidget.Slots[] -> PanelSlot.Content, and nested UserWidgets the same way.
// A TextBlock's words are its "Text" field (FText). Visibility = ESlateVisibility (0 Visible, 1 Collapsed, 2 Hidden,
// 3 HitTestInvisible, 4 SelfHitTestInvisible).
static uintptr_t obj_field(uintptr_t o, const char* f) { Prop p = find_prop(class_of(o), f); if (!p || p.size < 8) return 0; uintptr_t v = 0; rd_as(o + p.offset, v); return v; }
static int widget_vis(uintptr_t w) { Prop p = find_prop(class_of(w), "Visibility"); return p ? (int)get_num(w, p) : -1; }
static std::string widget_text(uintptr_t w) { Prop p = find_prop(class_of(w), "Text"); return (p && p.type == "TextProperty") ? ftext(w + p.offset) : std::string(); }
static std::vector<uintptr_t> widget_children(uintptr_t w) {
    std::vector<uintptr_t> out;
    if (uintptr_t tree = obj_field(w, "WidgetTree")) { if (uintptr_t root = obj_field(tree, "RootWidget")) out.push_back(root); }   // a UserWidget
    Prop sl = find_prop(class_of(w), "Slots");
    if (sl && sl.type == "ArrayProperty") { auto a = array_of(w, sl);
        for (int32_t i = 0; i < a.num && i < 512 && a.data; ++i) { uintptr_t s = 0; rd_as((uintptr_t)a.data + (uintptr_t)i * 8, s); if (s) if (uintptr_t c = obj_field(s, "Content")) out.push_back(c); } }
    // CommonUI stacks / queues (HUD_Stack, menu layers): the screens they hold are in WidgetList, not in Slots
    Prop wl = find_prop(class_of(w), "WidgetList");
    if (wl && wl.type == "ArrayProperty") { auto a = array_of(w, wl);
        for (int32_t i = 0; i < a.num && i < 64 && a.data; ++i) { uintptr_t c = 0; rd_as((uintptr_t)a.data + (uintptr_t)i * 8, c); if (c && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c); } }
    // HUD extension points (BottomLeft, TopCenter ...): the plugged-in widgets are the object values of their maps
    for (const char* mn : { "ExtensionMapping", "WidgetMapping" }) {
        Prop mp = find_prop(class_of(w), mn); if (!mp || mp.type != "MapProperty") continue;
        Prop k, v; int32_t vo = 0, st = 0; if (!map_kv(mp, k, v, vo, st) || !(v.type == "ObjectProperty" || v.type == "ObjectPtrProperty")) continue;
        for (auto& [ka, va] : map_entries(w, mp)) { uintptr_t c = 0; rd_as(va, c); if (c && alive(c) && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c); }
    }
    // DynamicEntryBox / lists: EntryWidgetPool.ActiveWidgets
    Prop pool = find_prop(class_of(w), "EntryWidgetPool");
    if (pool && pool.type == "StructProperty" && pool.owner_struct) { Prop aw = find_prop(pool.owner_struct, "ActiveWidgets");
        if (aw && aw.type == "ArrayProperty") { auto a = array_of(w + pool.offset, aw);
            for (int32_t i = 0; i < a.num && i < 256 && a.data; ++i) { uintptr_t c = 0; rd_as((uintptr_t)a.data + (uintptr_t)i * 8, c); if (c && std::find(out.begin(), out.end(), c) == out.end()) out.push_back(c); } } }
    return out;
}
static void widget_walk(uintptr_t w, int depth, int maxd, bool hidden_too, json& out, int& budget) {
    if (!w || budget <= 0 || depth > maxd || !alive(w)) return;
    const int vis = widget_vis(w); if (!hidden_too && (vis == 1 || vis == 2)) return;
    --budget;
    json e = { {"addr", hexstr(w)}, {"class", name_of(class_of(w))}, {"name", name_of(w)}, {"depth", depth} };
    if (vis >= 0) e["vis"] = vis;
    const std::string t = widget_text(w); if (!t.empty()) e["text"] = t;
    out.push_back(e);
    for (uintptr_t c : widget_children(w)) widget_walk(c, depth + 1, maxd, hidden_too, out, budget);
}
static bool widget_on_screen(uintptr_t w) {   // UserWidget::IsInViewport (top-level screens)
    Params p = params_for(w, "IsInViewport"); if (!p) return false; std::string e; return call(w, p, &e) && p.ret<bool>();
}
static bool obj_spec(const std::string& s, uintptr_t& ob);
static bool make_ftext(const std::string& s, uint8_t out[0x18], std::string* why);
static bool make_fstring(const std::string& s, uint8_t out[16], bool* ours = nullptr);
static bool is_numeric_type(const std::string& t);
static bool is_object_type(const std::string& t);
static bool def_from_string(const std::string& s, uint8_t* out, size_t n, std::string* why);
// Write one reflected field of an object or of a struct at `o` (ust = its class / script struct). value: number / bool, or
// text: FName, obj:|addr: (object), FString, FText, def:("name","Type") (GbxDefPtr). Returns before / after.
json field_set(uintptr_t o, uintptr_t ust, const std::string& field, const json& v) {
    uintptr_t base = 0; Prop leaf; if (!o || !ust || !resolve(o, ust, field, base, leaf)) return fail("NO_FIELD", "field not found: " + field);
    const uintptr_t at = base + leaf.offset;
    auto show = [&]() -> json { return leaf.type == "StrProperty" ? json(fstring(at)) : leaf.type == "TextProperty" ? json(ftext(at)) :
        leaf.type == "NameProperty" ? json(name_str(get<FName>(base, leaf))) : leaf.type == "GbxDefPtrProperty" ? json(def_text(at)) : json(get_num(base, leaf)); };
    const json before = show();
    if (v.is_number() || v.is_boolean()) {
        if (!is_numeric_type(leaf.type)) return fail("BAD_ARG", "a number does not fit " + leaf.type);
        const double d = v.is_boolean() ? (v.get<bool>() ? 1 : 0) : v.get<double>();
        if (!(leaf.type == "BoolProperty" ? set_bool(base, leaf, d != 0) : set_num(base, leaf, d))) return fail("WRITE", leaf.type);
    } else if (v.is_string()) {
        const std::string s = v.get<std::string>(); uintptr_t ob = 0; FName fn{};
        if (leaf.type == "NameProperty") { if (!name_find(s, fn)) return fail("NO_NAME", s); if (!mwrite(at, &fn, 8)) return fail("WRITE", "name"); }
        else if (is_object_type(leaf.type)) { if (!obj_spec(s, ob) || !ob) return fail("NOT_FOUND", s); if (!mwrite(at, &ob, 8)) return fail("WRITE", "object"); }
        else if (leaf.type == "StrProperty") { uint8_t fs[16]; if (!make_fstring(s, fs, nullptr) || !mwrite(at, fs, 16)) return fail("WRITE", "string"); }
        else if (leaf.type == "TextProperty") { uint8_t t[0x18]; std::string why; if (leaf.size < 8 || leaf.size > 0x18) return fail("TEXT", "text layout"); if (!make_ftext(s, t, &why)) return fail("TEXT", why); if (!mwrite(at, t, (size_t)leaf.size)) return fail("WRITE", "text"); }
        else if (leaf.type == "GbxDefPtrProperty" && leaf.size >= 0x18) { uint8_t t[0x18] = {}; std::string why; if (!def_from_string(s.rfind("def:", 0) == 0 ? s.substr(4) : s, t, sizeof t, &why)) return fail("NO_DEF", why); if (!mwrite(at, t, 0x18)) return fail("WRITE", "def"); }
        else return fail("BAD_ARG", "text does not fit " + leaf.type);
    } else return fail("BAD_ARG", "value: number, bool or text");
    return ok({ {"field", field}, {"type", leaf.type}, {"before", before}, {"after", show()}, {"at", hexstr(at)} });
}
json struct_json(uintptr_t base, uintptr_t ust, int depth, int maxd) { const int old = t_maxd; t_maxd = std::clamp(maxd, 1, 12); json r = struct_value(base, ust, depth); t_maxd = old; return r; }
json cmd(const std::string& name, const json& a) {
    if (name == "UE.TRACE") return trace::cmd(a);
    if (name == "UE.STATUS") { init(); return ok(status()); }
    if (!init()) return fail("NOT_READY", status().dump());
    return run_on_main([name, a]() -> json {
        if (name == "UE.DTROWS") {                     // {asset:"/Game/.../DT_X.DT_X", max} -> {struct, rows:{name:{...}}}
            const std::string path = a.value("asset", ""); const size_t dot = path.rfind('.');
            if (dot == std::string::npos) return fail("BAD_ARG", "asset = /Game/Path/Name.Name");
            uintptr_t dt = find_object(path, false);
            if (!dt) {
                FName pk{}, as{};
                if (!name_find(path.substr(0, dot), pk) || !name_find(path.substr(dot + 1), as)) return fail("NO_NAME", "asset name not in the name pool");
                uintptr_t k = cdo_of(find_class("KismetSystemLibrary")); Params p = params_for(k, "LoadAsset_Blocking"); if (!p) return fail("NO_FUNC", "LoadAsset_Blocking");
                Prop sp = p.param("Asset"); if (!sp || sp.size < 0x18) return fail("NO_FUNC", "LoadAsset_Blocking has no usable Asset parameter");
                memcpy(p.at(sp) + 8, &pk, 8); memcpy(p.at(sp) + 0x10, &as, 8); std::string e; if (!call(k, p, &e)) return fail("LOAD", e);
                dt = p.ret<uintptr_t>();
            }
            if (!dt) return fail("NOT_FOUND", "asset did not load");
            uintptr_t rs = 0, data = 0; int32_t num = 0; rd_as(dt + 0x28, rs); rd_as(dt + 0x30, data); rd_as(dt + 0x38, num);
            if (!rs || !data || num < 0 || num > 20000) return fail("LAYOUT", "not a data table layout");
            json rows = json::object(); const int mx = a.value("max", 200);
            for (int32_t i = 0; i < num && i < mx; ++i) {
                FName n{}; uintptr_t row = 0; if (!mread(data + i * 24, &n, 8) || !rd_as(data + i * 24 + 8, row) || !row) continue;
                rows[name_str(n)] = struct_value(row, rs, 0);
            }
            return ok({ {"struct", name_of(rs)}, {"count", num}, {"rows", rows} });
        }
        if (name == "UE.DUMP") {                       // {dir, pkg:"" (substring of the package path; "" = every /Script/ package)}
            // offset/size/type) + <dir>\functions.tsv (package, class, function, flags, params with types, return).
            const std::string dir = a.value("dir", ""), pkgf = a.value("pkg", "");
            if (dir.empty()) return fail("BAD_ARG", "dir");
            CreateDirectoryA(dir.c_str(), nullptr);
            std::ofstream fc(dir + "\\classes.txt"), ff(dir + "\\functions.tsv");
            if (!fc || !ff) return fail("IO", "cannot write to " + dir);
            ff << "package\tclass\tfunction\tflags\tparams\treturn\n";
            int nc = 0, nf = 0, ns = 0; const int32_t n = num_objects();
            for (int32_t i = 0; i < n; ++i) {
                uintptr_t o = object_at(i); if (!o) continue;
                uintptr_t k = class_of(o); const bool is_cls = k == g_cls_class, is_st = g_cls_scriptstruct && k == g_cls_scriptstruct;
                if (!is_cls && !is_st) continue;
                const std::string pkg = path_of(outer_of(o));
                if (pkg.rfind("/Script/", 0) != 0) continue;
                if (!pkgf.empty() && pkg.find(pkgf) == std::string::npos) continue;
                int32_t size = 0; rd_as(o + off::Struct_Size, size);
                const uintptr_t sup = super_of(o);
                fc << (is_cls ? "class " : "struct ") << pkg << "." << name_of(o) << " : " << (sup ? name_of(sup) : "-") << "  size " << hexstr(size) << "\n";
                for (auto& p : props(o, false)) {
                    fc << "    " << hexstr(p.offset) << "  " << p.size << "  " << p.type;
                    if (p.owner_struct) fc << "<" << name_of(p.owner_struct) << ">";
                    fc << "  " << p.name << "\n";
                }
                if (is_st) { ++ns; continue; }
                ++nc;
                for (uintptr_t f : funcs(o, false)) {
                    uint32_t fl = 0; rd_as(f + off::Func_Flags, fl);
                    std::string ps, ret = "void";
                    for (auto& p : props(f, false)) {
                        std::string t = p.type; if (p.owner_struct) t += "<" + name_of(p.owner_struct) + ">";
                        if (p.flags & CPF_ReturnParm) { ret = t; continue; }
                        if (!ps.empty()) ps += ", ";
                        ps += ((p.flags & CPF_OutParm) ? "out " : "") + t + " " + p.name;
                    }
                    ff << pkg << "\t" << name_of(o) << "\t" << name_of(f) << "\t" << hexstr(fl) << "\t" << ps << "\t" << ret << "\n"; ++nf;
                }
            }
            return ok({ {"classes", nc}, {"structs", ns}, {"functions", nf}, {"dir", dir} });
        }
        if (name == "UE.VALUE") {                      // {obj..., field: "a.b", max: 200} -> deep value (arrays of structs, enums, soft paths)
            uintptr_t ob = obj_arg(a); if (!ob) return fail("NOT_FOUND", "no object");
            uintptr_t base = ob; Prop leaf;
            const std::string fld = a.value("field", "");
            if (fld.empty()) return ok(struct_value(ob, class_of(ob), 0));
            if (!resolve(ob, class_of(ob), fld, base, leaf)) return fail("NO_FIELD", "field not found");
            const int mx = a.value("max", 200);
            if (leaf.type == "ArrayProperty") {
                Prop in = array_inner(leaf); TArray<uint8_t> arr = array_of(base, leaf);
                json out = json::array();
                for (int32_t i = 0; i < arr.num && i < mx; ++i) {
                    const uintptr_t el = (uintptr_t)arr.data + (uintptr_t)i * in.size;
                    Prop at0 = in; at0.offset = 0;
                    if (in.type == "StructProperty" && in.owner_struct) out.push_back(struct_value(el, in.owner_struct, 0));
                    else out.push_back(prop_value(el, at0, 0));
                }
                return ok({ {"num", arr.num}, {"items", out} });
            }
            if (leaf.type == "StructProperty" && leaf.owner_struct) return ok(struct_value(base + leaf.offset, leaf.owner_struct, 0));
            return ok(prop_value(base, leaf, 0));
        }
        if (name == "UE.NAMEOF") {                     // {index, number} -> text (stored name IDs back to text)
            FName n{}; n.index = (uint32_t)a.value("index", 0); n.number = (uint32_t)a.value("number", 0);
            return ok(name_str(n));
        }
        if (name == "UE.FIND") {                       // {class | name, max}
            json arr = json::array(); const size_t max = a.value("max", 30);
            if (a.contains("class")) { for (uintptr_t o : find_all(find_class(a["class"]), a.value("cdo", false), max)) arr.push_back({ {"addr", hexstr(o)}, {"name", full_name(o)} }); }
            else if (a.contains("name")) { for (uintptr_t o : objects_named(a["name"])) { arr.push_back({ {"addr", hexstr(o)}, {"name", full_name(o)} }); if (arr.size() >= max) break; } }
            return ok(arr);
        }
        uintptr_t o = obj_arg(a);
        if (name == "UE.OBJ") return o ? ok({ {"addr", hexstr(o)}, {"name", full_name(o)}, {"class", path_of(class_of(o))}, {"outer", path_of(outer_of(o))}, {"cdo", is_cdo(o)} }) : fail("NOT_FOUND", "no object");
        if (name == "UE.PROPS") {                      // {class|path|addr, filter, super}
            uintptr_t s = a.contains("class") ? find_class(a["class"]) : (a.contains("struct") ? find_struct(a["struct"]) : class_of(o));
            if (!s) return fail("NOT_FOUND", "no class/struct");
            const std::string flt = a.value("filter", "");
            json arr = json::array(); for (auto& p : props(s, a.value("super", true))) if (flt.empty() || p.name.find(flt) != std::string::npos) arr.push_back(prop_json(p));
            return ok(arr);
        }
        if (name == "UE.FUNCS") {
            uintptr_t c = a.contains("class") ? find_class(a["class"]) : class_of(o); if (!c) return fail("NOT_FOUND", "no class");
            const std::string flt = a.value("filter", "");
            json arr = json::array();
            for (uintptr_t f : funcs(c, a.value("super", false))) {
                const std::string n = name_of(f); if (!flt.empty() && n.find(flt) == std::string::npos) continue;
                json ps = json::array(); for (auto& p : props(f, false)) ps.push_back(prop_json(p));
                arr.push_back({ {"name", n}, {"params", ps} });
            }
            return ok(arr);
        }
        if (name == "UE.GET") {                        // {obj..., field: "a.b.c"}
            if (!o) return fail("NOT_FOUND", "no object");
            uintptr_t base = 0; Prop leaf; if (!resolve(o, class_of(o), a.value("field", ""), base, leaf)) return fail("NO_FIELD", "field not found");
            json r = prop_json(leaf); r["value"] = get_num(base, leaf); r["at"] = hexstr(base + leaf.offset);
            if (leaf.type == "ObjectProperty") r["value_obj"] = full_name(get<uintptr_t>(base, leaf));
            if (leaf.type == "NameProperty") r["value_name"] = name_str(get<FName>(base, leaf));
            if (leaf.type == "StrProperty") r["value_str"] = fstring(base + leaf.offset);
            if (leaf.type == "TextProperty") r["value_text"] = ftext(base + leaf.offset);
            if (leaf.type == "ArrayProperty") r["num"] = array_of(base, leaf).num;
            if (leaf.type == "MapProperty") r["num"] = map_entries(base, leaf).size();
            return ok(r);
        }
        if (name == "UE.WIDGETS") {                    // {all:false, filter, max}: the game's screens that are on screen now
            uintptr_t uw = find_class("UserWidget"); if (!uw) return fail("NO_UMG", "UserWidget class not found");
            const std::string f = a.value("filter", std::string()); const bool all = a.value("all", false); const size_t mx = a.value("max", 200);
            json out = json::array();
            for (uintptr_t w : find_all(uw, false)) {
                const std::string cn = name_of(class_of(w));
                if (!f.empty()) { std::string l1 = cn, l2 = f; for (auto& c : l1) c = (char)tolower(c); for (auto& c : l2) c = (char)tolower(c); if (l1.find(l2) == std::string::npos) continue; }
                const bool on = widget_on_screen(w); if (!all && !on) continue;
                out.push_back({ {"addr", hexstr(w)}, {"class", cn}, {"name", name_of(w)}, {"on_screen", on}, {"vis", widget_vis(w)} });
                if (out.size() >= mx) break;
            }
            return ok({ {"count", out.size()}, {"widgets", out} });
        }
        if (name == "UE.WIDGET_TREE") {                // {addr, depth:12, hidden:false, max:600}: every element of one screen + its words
            if (!o) return fail("NOT_FOUND", "no widget");
            json out = json::array(); int budget = a.value("max", 600);
            widget_walk(o, 0, a.value("depth", 12), a.value("hidden", false), out, budget);
            return ok({ {"count", out.size()}, {"tree", out} });
        }
        if (name == "UE.FIND_TEXT") {                  // {text, max:40}: which on-screen element shows these words
            const std::string q0 = a.value("text", std::string()); if (q0.empty()) return fail("BAD_ARG", "text");
            std::string q = q0; for (auto& c : q) c = (char)tolower(c);
            uintptr_t uw = find_class("UserWidget"); if (!uw) return fail("NO_UMG", "UserWidget class not found");
            json hits = json::array(); const size_t mx = a.value("max", 40);
            for (uintptr_t w : find_all(uw, false)) {
                if (!a.value("all", true) && !widget_on_screen(w)) continue;   // world labels are not "in the viewport" - searched too
                json tree = json::array(); int budget = 1500; widget_walk(w, 0, 20, a.value("hidden", false), tree, budget);
                for (auto& e : tree) { std::string t = e.value("text", std::string()); std::string lt = t; for (auto& c : lt) c = (char)tolower(c);
                    if (!t.empty() && lt.find(q) != std::string::npos) { e["screen"] = name_of(class_of(w)); e["screen_addr"] = hexstr(w); hits.push_back(e); if (hits.size() >= mx) break; } }
                if (hits.size() >= mx) break;
            }
            return ok({ {"count", hits.size()}, {"hits", hits} });
        }
        if (name == "UE.STRUCT") {                     // {addr, struct?, field?}: a struct / Gearbox object at an address (struct: its own type)
            uintptr_t at = 0; try { at = std::stoull(a.value("addr", std::string("0")), nullptr, 16); } catch (...) {}
            if (!at) return fail("BAD_ARG", "addr");
            uintptr_t st = a.contains("struct") ? find_struct(a.value("struct", std::string())) : typed_struct_of(at);
            if (!st) return fail("NO_STRUCT", "struct type unknown (give struct)");
            const std::string fld = a.value("field", std::string());
            const int old = t_maxd; t_maxd = std::clamp(a.value("depth", 4), 1, 12);
            if (fld.empty()) { json o = struct_value(at, st, 0); o["@type"] = name_of(st); t_maxd = old; return ok(o); }
            uintptr_t b = 0; Prop leaf; if (!resolve(at, st, fld, b, leaf)) return fail("NO_FIELD", fld);
            json v = prop_value(b, leaf, 0); t_maxd = old;
            return ok({ {"field", fld}, {"type", leaf.type}, {"value", v}, {"at", hexstr(b + leaf.offset)} });
        }
        if (name == "UE.SET") {                        // {obj..., field: "a.b.c", value}: number / bool / name / obj:|addr: / string / text / def:
            if (!o) return fail("NOT_FOUND", "no object");
            if (!a.contains("value")) return fail("BAD_ARG", "value");
            return field_set(o, class_of(o), a.value("field", ""), a["value"]);
        }
        if (name == "UE.CALL") {                       // {obj..., func, args: {param: number | "obj:<path>" | "name:<FName>" | "class:<name>"}}
            if (!o) return fail("NOT_FOUND", "no object");
            return call_json(o, a.value("func", ""), a.contains("args") ? a["args"] : json::object(), 0);
        }
        if (name == "UE.NAME") { FName n; bool f = name_find(a.value("s", ""), n); return ok({ {"found", f}, {"index", n.index}, {"number", n.number}, {"back", name_str(n)} }); }
        return fail("UNKNOWN", "unknown UE command " + name);
    }, 8000);
}

// FString built in GAME memory (GMalloc): a by-value FString parameter may be moved/freed by the callee, so the buffer
// must be one the game's allocator owns. {Data wchar_t*, Num (incl. 0), Max}. Never freed by us (tiny, rare).
// *ours = the buffer came from our own heap (GMalloc not found).
static bool make_fstring(const std::string& s, uint8_t out[16], bool* ours) {
    memset(out, 0, 16); if (ours) *ours = false;
    if (s.empty()) return true;
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), nullptr, 0);
    wchar_t* buf = (wchar_t*)game_realloc(nullptr, (size_t)(n + 1) * 2, 8);
    // BL4: GMalloc not found -> our own heap. Safe for by-value / const& parameters: ProcessEvent's caller owns the
    // parameter block (the callee copies into its own FString). Leaked on purpose by call_json (never freed by the game
    // either); our own helper calls below free it after the call when the parameter still points at it.
    if (!buf) { buf = (wchar_t*)HeapAlloc(GetProcessHeap(), 0, (size_t)(n + 1) * 2); if (buf && ours) *ours = true; }
    if (!buf) return false;
    MultiByteToWideChar(CP_UTF8, 0, s.data(), (int)s.size(), buf, n); buf[n] = 0;
    const int32_t num = n + 1; memcpy(out, &buf, 8); memcpy(out + 8, &num, 4); memcpy(out + 12, &num, 4);
    return true;
}
// Helper calls made while arguments are prepared / results are read are not journaled: a game that is closed during
// one of them must not get a library function (IsValidGameDataHandle, the def converters) blocked for good.
struct NoJournal { bool was; NoJournal() : was(t_journal) { t_journal = false; } ~NoJournal() { t_journal = was; } };
// CDO of a function library, resolved once (validated on every use)
static uintptr_t lib_cdo(const char* cls) {
    static std::mutex mx; static std::map<std::string, uintptr_t> cache;
    { std::lock_guard<std::mutex> lk(mx); auto it = cache.find(cls); if (it != cache.end() && alive(it->second)) return it->second; }
    const uintptr_t d = cdo_of(find_class(cls));
    if (d) { std::lock_guard<std::mutex> lk(mx); cache[cls] = d; }
    return d;
}
// Gearbox defs (GbxDefPtrProperty, 0x18 bytes) <-> text through the game's own converters (KismetStringLibrary).
// Both directions are cached for a short time (text -> bytes, bytes -> text): readouts that pass the same def every
// second do not call the game (and allocate) every second.
struct DefHit { std::array<uint8_t, 0x18> bytes{}; std::string text; double at = 0; };
static std::mutex g_def_mtx;
static std::map<std::string, DefHit> g_def_cache;        // def text -> bytes
static std::map<std::string, DefHit> g_def_text_cache;   // type initial + bytes -> text
static const double kDefCacheMs = 30000;
static bool def_from_string(const std::string& s, uint8_t* out, size_t n, std::string* why) {
    const double now = now_ms();
    { std::lock_guard<std::mutex> lk(g_def_mtx);
      auto it = g_def_cache.find(s);
      if (it != g_def_cache.end() && now - it->second.at < kDefCacheMs) { memcpy(out, it->second.bytes.data(), std::min<size_t>(n, 0x18)); return true; } }
    NoJournal nj;
    const uintptr_t ksl = lib_cdo("KismetStringLibrary");
    Params p = params_for(ksl, "SetGbxDefPtrFromString"); if (!p) { if (why) *why = "SetGbxDefPtrFromString missing"; return false; }
    Prop vp = p.param("Value"), d = p.param("def");
    if (!vp || vp.size < 16) { if (why) *why = "SetGbxDefPtrFromString has no Value parameter"; return false; }
    if (!d || d.size <= 0) { if (why) *why = "no def param"; return false; }
    uint8_t fs[16]; bool ours = false; if (!make_fstring(s, fs, &ours)) { if (why) *why = "allocation failed"; return false; }
    memcpy(p.at(vp), fs, 16);
    const bool called = call(ksl, p, why);
    if (ours && memcmp(p.at(vp), fs, 8) == 0) {          // the callee only read it (const FString&): our buffer, ours to free
        void* b = nullptr; memcpy(&b, fs, 8); if (b) HeapFree(GetProcessHeap(), 0, b);
    }
    if (!called) return false;
    memcpy(out, p.at(d), std::min<size_t>(n, (size_t)d.size));
    bool any = false; for (int i = 0; i < d.size; ++i) any |= p.at(d)[i] != 0;
    if (!any && why) *why = "unknown def '" + s + "' (SetGbxDefPtrFromString left it empty)";
    if (any && d.size <= 0x18) {
        DefHit h; memcpy(h.bytes.data(), p.at(d), (size_t)d.size); h.at = now;
        std::lock_guard<std::mutex> lk(g_def_mtx);
        if (g_def_cache.size() > 5000) g_def_cache.clear();
        g_def_cache[s] = h;
    }
    return any;
}
// Gearbox GameDataHandle (0x18) = {FName name @0, uint32 category @8, rest 0}. The category is found by asking the game
// (KismetSystemLibrary::IsValidGameDataHandle) for categories 0..16383; cached per name (misses too, for 30 s).
// "gdh:<name>" or "gdh:<name>:<cat>".
static std::map<std::string, int> g_gdh_cat;
static std::map<std::string, double> g_gdh_bad;
static bool gdh_from(const std::string& spec, uint8_t out[0x18], std::string* why) {
    std::string nm = spec; int cat = -1;
    const size_t k = spec.rfind(':');
    if (k != std::string::npos && k + 1 < spec.size() && isdigit((unsigned char)spec[k + 1])) { nm = spec.substr(0, k); cat = (int)strtol(spec.c_str() + k + 1, nullptr, 10); }
    FName fn; if (!name_find(nm, fn)) { if (why) *why = "name not in the pool: " + nm; return false; }
    memset(out, 0, 0x18); memcpy(out, &fn, 8);
    if (cat < 0) { auto it = g_gdh_cat.find(nm); if (it != g_gdh_cat.end()) cat = it->second; }
    if (cat >= 0) { memcpy(out + 8, &cat, 4); return true; }
    const double now = now_ms();
    { auto it = g_gdh_bad.find(nm);
      if (it != g_gdh_bad.end() && now - it->second < 30000) { if (why) *why = "no game-data category accepts '" + nm + "' (checked a moment ago)"; return false; } }
    // one lookup of the library + function for the whole search, no crash journal (file IO) per probe
    NoJournal nj;
    const uintptr_t ksl = lib_cdo("KismetSystemLibrary");
    Params p = params_for(ksl, "IsValidGameDataHandle");
    Prop hp = p ? p.param("Handle") : Prop();
    if (!p || !hp || hp.size < 0x18) { if (why) *why = "IsValidGameDataHandle is not available"; return false; }
    std::string e;
    for (int c = 0; c < 16384; ++c) {
        memcpy(out + 8, &c, 4);
        std::fill(p.buf.begin(), p.buf.end(), (uint8_t)0); memcpy(p.at(hp), out, 0x18);
        if (!call(ksl, p, &e)) { memset(out + 8, 0, 4); if (why) *why = "IsValidGameDataHandle failed: " + e; return false; }   // every further probe would fail the same way
        if (p.ret<bool>()) { g_gdh_cat[nm] = c; return true; }
    }
    memset(out + 8, 0, 4);
    if (g_gdh_bad.size() > 2000) g_gdh_bad.clear();
    g_gdh_bad[nm] = now;
    if (why) *why = "no game-data category accepts '" + nm + "' (IsValidGameDataHandle false for 0..16383)";
    return false;
}
static std::string def_to_string(const uint8_t* def, const std::string& type) {
    uint8_t raw[0x18] = {}; if (!def || !mread((uintptr_t)def, raw, sizeof raw)) return "";
    const std::string key = type.substr(0, 2) + std::string((const char*)raw, sizeof raw);
    const double now = now_ms();
    { std::lock_guard<std::mutex> lk(g_def_mtx);
      auto it = g_def_text_cache.find(key);
      if (it != g_def_text_cache.end() && now - it->second.at < kDefCacheMs) return it->second.text; }
    NoJournal nj;
    const uintptr_t ksl = lib_cdo("KismetStringLibrary");
    const char* fn = type == "GameDataHandleProperty" ? "Conv_GameDataHandleToString" : "Conv_GbxDefPtrToString";
    Params p = params_for(ksl, fn); if (!p) return "";
    auto ps = props(p.fn, false); if (ps.empty() || ps[0].type != type || ps[0].size <= 0) return "";
    memcpy(p.at(ps[0]), raw, std::min<size_t>((size_t)ps[0].size, 0x18));
    std::string e; if (!call(ksl, p, &e)) return "";
    uint8_t* r = p.ret_ptr(); std::string out = r ? fstring((uintptr_t)r) : "";   // (engine FString: not ours to free)
    DefHit h; h.text = out; h.at = now;
    std::lock_guard<std::mutex> lk(g_def_mtx);
    if (g_def_text_cache.size() > 20000) g_def_text_cache.clear();
    g_def_text_cache[key] = h;
    return out;
}
bool def_bytes(const std::string& text, uint8_t out[0x18], std::string* why) { memset(out, 0, 0x18); return def_from_string(text, out, 0x18, why); }
std::string def_text(uintptr_t at) { uint8_t b[0x18]; if (!mread(at, b, sizeof b)) return ""; return def_to_string(b, "GbxDefPtrProperty"); }

// ---- argument forms shared by struct fields, array elements and parameters
static bool is_numeric_type(const std::string& t) {
    return t == "BoolProperty" || t == "FloatProperty" || t == "DoubleProperty" || t == "IntProperty" || t == "Int64Property" || t == "UInt32Property" ||
           t == "UInt64Property" || t == "UInt16Property" || t == "Int16Property" || t == "ByteProperty" || t == "Int8Property" || t == "EnumProperty";
}
static bool is_object_type(const std::string& t) {
    return t == "ObjectProperty" || t == "ClassProperty" || t == "InterfaceProperty" || t == "ObjectPtrProperty" || t == "ClassPtrProperty";
}
// "addr:<expr>" | "obj:<path>" | "class:<C>" | "first:<C>" -> true when s is one of these forms (ob = 0: not found)
static bool obj_spec(const std::string& s, uintptr_t& ob) {
    ob = 0;
    if (s.rfind("addr:", 0) == 0) { std::string e; eval_addr(s.substr(5), ob, &e); return true; }
    if (s.rfind("obj:", 0) == 0) { ob = find_object(s.substr(4)); return true; }
    if (s.rfind("class:", 0) == 0) { ob = find_class(s.substr(6)); return true; }
    if (s.rfind("first:", 0) == 0) { ob = find_first(find_class(s.substr(6))); return true; }
    return false;
}
static bool put_number(uint8_t* base, const Prop& f, const json& v) {
    const double d = v.is_boolean() ? (v.get<bool>() ? 1 : 0) : v.get<double>();
    return f.type == "BoolProperty" ? set_bool((uintptr_t)base, f, d != 0) : set_num((uintptr_t)base, f, d);
}

static bool struct_put(uint8_t* base, uintptr_t ustruct, const json& v, std::string* why, int depth = 0);
// TArray from JSON written at dst: elements are structs (objects), names / objects (strings) or numbers. The buffer comes
// from the game allocator when found, else our heap (leaked on purpose: read-only use by the callee).
static bool array_put(uint8_t* dst, const Prop& f, const json& arr, const std::string& key, std::string* why, int depth) {
    auto W = [&](const std::string& m) { if (why) *why = m; return false; };
    if (f.type != "ArrayProperty" || f.size < 16) return W(key + " is not an array (" + f.type + ")");
    Prop in = array_inner(f); if (!in || in.size <= 0) return W(key + ": unknown array element type");
    const size_t n = arr.size();
    if (n > 100000) return W(key + ": too many elements");
    uint8_t* buf = nullptr; bool ours = false;
    if (n) {
        buf = (uint8_t*)game_realloc(nullptr, n * in.size, 16);
        if (!buf) { buf = (uint8_t*)HeapAlloc(GetProcessHeap(), HEAP_ZERO_MEMORY, n * in.size); ours = true; }
        if (!buf) return W("allocation failed");
        memset(buf, 0, n * in.size);
    }
    auto bad = [&](const std::string& m) { if (why && !m.empty()) *why = m; if (ours && buf) HeapFree(GetProcessHeap(), 0, buf); return false; };
    Prop at0 = in; at0.offset = 0;
    for (size_t i = 0; i < n; ++i) {
        uint8_t* el = buf + i * in.size; const json& e = arr[i];
        const std::string where = key + "[" + std::to_string(i) + "]";
        if (e.is_object() && in.type == "StructProperty" && in.owner_struct) { if (!struct_put(el, in.owner_struct, e, why, depth + 1)) return bad(""); }
        else if (e.is_string() && in.type == "NameProperty" && in.size >= 8) {
            FName nm; if (!name_find(e.get<std::string>(), nm)) return bad("name not in pool: " + e.get<std::string>());
            memcpy(el, &nm, 8); }
        else if (e.is_string() && is_object_type(in.type) && in.size >= 8) {
            uintptr_t ob = 0; if (!obj_spec(e.get<std::string>(), ob) || !ob) return bad(where + ": object not found: " + e.get<std::string>());
            memcpy(el, &ob, 8); }
        else if ((e.is_number() || e.is_boolean()) && is_numeric_type(in.type)) { if (!put_number(el, at0, e)) return bad(where + ": cannot write " + in.type); }
        else return bad(where + ": unsupported element for " + in.type);
    }
    const int32_t num = (int32_t)n; memcpy(dst, &buf, 8); memcpy(dst + 8, &num, 4); memcpy(dst + 12, &num, 4);
    return true;
}

// JSON object -> struct fields by name (numbers, bools, nested structs), e.g. Transform {"Translation":{"X":1}}.
// Every value is checked against the field's type: a value that does not fit is an error, never a partial write.
static bool struct_put(uint8_t* base, uintptr_t ustruct, const json& v, std::string* why, int depth) {
    auto W = [&](const std::string& m) { if (why) *why = m; return false; };
    if (depth > 6) return W("struct nesting too deep");
    if (!v.is_object()) return W("a struct needs a JSON object");
    for (auto& kv : v.items()) {
        Prop f = find_prop(ustruct, kv.key());
        if (!f) return W("no field " + kv.key() + " in " + name_of(ustruct));
        const json& x = kv.value();
        if (x.is_null()) continue;
        if (x.is_object()) {
            if (f.type != "StructProperty" || !f.owner_struct) return W(kv.key() + " is not a struct");
            if (!struct_put(base + f.offset, f.owner_struct, x, why, depth + 1)) return false;
        } else if (x.is_array()) {
            if (!array_put(base + f.offset, f, x, kv.key(), why, depth)) return false;
        } else if (x.is_string() && (f.type == "GameDataHandleProperty" || f.type == "GbxDefPtrProperty")) {   // "gdh:..." / "def:..." inside structs
            const std::string s = x.get<std::string>(); uint8_t tmp[0x18] = {};
            if (s.rfind("gdh:", 0) == 0) { if (!gdh_from(s.substr(4), tmp, why)) return false; }
            else if (s.rfind("def:", 0) == 0) { if (!def_from_string(s.substr(4), tmp, sizeof tmp, why)) return false; }
            else return W(kv.key() + ": use gdh:<name> or def:<text>");
            memcpy(base + f.offset, tmp, (size_t)std::clamp(f.size, 0, 0x18));
        } else if (x.is_number() || x.is_boolean()) {
            if (!is_numeric_type(f.type)) return W(kv.key() + ": a number does not fit " + f.type);
            if (!put_number(base, f, x)) return W(kv.key() + ": cannot write " + f.type);
        } else if (x.is_string() && f.type == "NameProperty" && f.size >= 8) {
            FName n; if (!name_find(x.get<std::string>(), n)) return W("name not in pool: " + x.get<std::string>());
            memcpy(base + f.offset, &n, 8);
        } else if (x.is_string() && is_object_type(f.type) && f.size >= 8) {   // object field: "addr:<hex>" | "obj:<path>" | "class:<C>" | "first:<C>"
            const std::string s = x.get<std::string>(); uintptr_t ob = 0;
            if (!obj_spec(s, ob) || !ob) return W("object not found for " + kv.key() + ": " + s);
            memcpy(base + f.offset, &ob, 8);
        } else return W("unsupported value for " + kv.key() + " (" + f.type + ")");
    }
    return true;
}

// Call a game function with JSON arguments (GAME THREAD). args values: number | bool | "obj:<path>" | "class:<name>" |
// "first:<class>" | "addr:<expr>" | "hex:<bytes>" | "name:<FName>" | "def:<text>" (GbxDefPtr / GameDataHandle via the
// game's SetGbxDefPtrFromString) | plain text for a StrProperty ("str:<text>" forces it) | {..} for a struct | [..] for
// an array. Every value is checked against the parameter's type and size (BAD_ARG, nothing is called); JSON null means
// "not given". world_ctx fills a WorldContextObject parameter that was not given.
// Result: {called, on, <out/return params>} (objects also as "addr").
// FText from plain text: the engine's KismetTextLibrary::Conv_StringToText builds it; its returned FText is copied out and
// one reference is kept on purpose (never released: a few bytes per call, and the text stays valid wherever it goes).
static bool make_ftext(const std::string& s, uint8_t out[0x18], std::string* why) {
    static uintptr_t lib = 0; if (!lib || !alive(lib)) lib = cdo_of(find_class("KismetTextLibrary"));
    if (!lib) { if (why) *why = "KismetTextLibrary missing"; return false; }
    Params p = params_for(lib, "Conv_StringToText"); if (!p) { if (why) *why = "Conv_StringToText missing"; return false; }
    Prop in = p.param("InString"), rv = p.param("ReturnValue");
    if (!in || !rv || rv.size < 8 || rv.size > 0x18 || in.size < 16) { if (why) *why = "Conv_StringToText layout"; return false; }
    uint8_t fs[16]; if (!make_fstring(s, fs)) { if (why) *why = "string allocation failed"; return false; }
    memcpy(p.at(in), fs, 16);
    std::string e; if (!call(lib, p, &e)) { if (why) *why = "Conv_StringToText: " + e; return false; }
    memset(out, 0, 0x18); memcpy(out, p.at(rv), (size_t)rv.size); return true;   // FText is 0x10 in UE 5.5
}
json call_json(uintptr_t o, const std::string& func, const json& args, uintptr_t world_ctx) {
    {
        {
            Params p = params_for(o, func); if (!p) return fail("NO_FUNC", "function not found: " + func);
            if (world_ctx && !args.contains("WorldContextObject")) { Prop wc = p.param("WorldContextObject"); if (wc && wc.size >= 8 && is_object_type(wc.type)) memcpy(p.at(wc), &world_ctx, 8); }
            if (!args.is_null() && !args.is_object()) return fail("BAD_ARG", "args must be an object {parameter: value}");
            for (auto& kv : args.items()) {
                Prop pp = p.param(kv.key()); if (!pp) return fail("NO_PARAM", "no parameter " + kv.key());
                const json& v = kv.value();
                auto bad = [&](const std::string& m) { return fail("BAD_ARG", kv.key() + ": " + m); };
                if (pp.size <= 0 || pp.offset < 0 || (size_t)pp.offset + (size_t)pp.size > p.buf.size()) return bad("parameter layout not readable");
                if (v.is_null()) continue;                        // not given
                if (v.is_object()) {                              // struct argument from a JSON object
                    if (pp.type != "StructProperty" || !pp.owner_struct) return bad("is not a struct parameter (" + pp.type + ")");
                    std::string why; if (!struct_put(p.at(pp), pp.owner_struct, v, &why)) return bad(why);
                    continue;
                }
                if (v.is_array()) {                               // array argument
                    std::string why; if (!array_put(p.at(pp), pp, v, kv.key(), &why, 0)) return bad(why);
                    continue;
                }
                if (v.is_number() || v.is_boolean()) {
                    if (!is_numeric_type(pp.type)) return bad("a number does not fit " + pp.type);
                    if (!put_number(p.buf.data(), pp, v)) return bad("cannot write " + pp.type);
                    continue;
                }
                if (!v.is_string()) return bad("unsupported value");
                const std::string s = v.get<std::string>();
                uintptr_t ob = 0; FName fnm;
                if (obj_spec(s, ob)) {                            // obj: / class: / first: / addr:
                    const bool raw64 = s.rfind("addr:", 0) == 0 && (pp.type == "Int64Property" || pp.type == "UInt64Property");
                    if ((!is_object_type(pp.type) && !raw64) || pp.size < 8) return bad("an object does not fit " + pp.type);
                    if (!ob) return fail("NOT_FOUND", "argument object not found: " + s);
                    memcpy(p.at(pp), &ob, 8);
                } else if (s.rfind("hex:", 0) == 0) {             // raw parameter bytes (structs), e.g. "hex:A0B1..."
                    const std::string h = s.substr(4);
                    if (h.size() % 2 || h.find_first_not_of("0123456789abcdefABCDEF") != std::string::npos) return bad("hex: needs pairs of hex digits");
                    if (h.size() / 2 > (size_t)pp.size) return bad("hex: " + std::to_string(h.size() / 2) + " bytes do not fit " + std::to_string(pp.size));
                    std::vector<uint8_t> b(h.size() / 2);
                    for (size_t i = 0; i < b.size(); ++i) b[i] = (uint8_t)strtoul(h.substr(i * 2, 2).c_str(), nullptr, 16);
                    if (!b.empty()) memcpy(p.at(pp), b.data(), b.size());
                } else if (s.rfind("name:", 0) == 0) {
                    if (pp.type != "NameProperty" || pp.size < 8) return bad("a name does not fit " + pp.type);
                    if (!name_find(s.substr(5), fnm)) return fail("NO_NAME", "name not in the pool: " + s.substr(5));
                    memcpy(p.at(pp), &fnm, 8);
                } else if (s.rfind("gdh:", 0) == 0) {             // Gearbox GameDataHandle by name
                    if (pp.size < 0x18) return bad("a game-data handle (0x18 bytes) does not fit " + pp.type);
                    std::string why; uint8_t h[0x18];
                    if (!gdh_from(s.substr(4), h, &why)) return fail("NO_HANDLE", why);
                    memcpy(p.at(pp), h, 0x18);
                } else if (s.rfind("def:", 0) == 0) {             // Gearbox def by its text form
                    if (pp.size < 0x18) return bad("a def (0x18 bytes) does not fit " + pp.type);
                    std::string why; uint8_t tmp[0x18] = {};
                    if (!def_from_string(s.substr(4), tmp, sizeof tmp, &why)) return fail("NO_DEF", why);
                    memcpy(p.at(pp), tmp, 0x18);
                } else if (pp.type == "StrProperty" && pp.size >= 16) {   // plain text (or "str:<text>")
                    uint8_t fs[16]; if (!make_fstring(s.rfind("str:", 0) == 0 ? s.substr(4) : s, fs)) return fail("ALLOC", "string allocation failed");
                    memcpy(p.at(pp), fs, 16);
                } else if (pp.type == "NameProperty" && pp.size >= 8) {   // plain text for an FName parameter
                    if (!name_find(s, fnm)) return fail("NO_NAME", "name not in the pool: " + s);
                    memcpy(p.at(pp), &fnm, 8);
                } else if (pp.type == "TextProperty" && pp.size >= 8 && pp.size <= 0x18) {   // display text (FText), e.g. TextBlock::SetText
                    uint8_t t[0x18]; std::string why;
                    if (!make_ftext(s.rfind("text:", 0) == 0 ? s.substr(5) : s, t, &why)) return fail("TEXT", why);
                    memcpy(p.at(pp), t, (size_t)pp.size);
                } else return bad("text does not fit " + pp.type + " (forms: obj: class: first: addr: name: hex: gdh: def:)");
            }
            std::string err;
            if (!call(o, p, &err)) return fail("CALL_FAILED", err);
            json r = { {"called", path_of(p.fn)}, {"on", full_name(o)} };
            for (auto& pp : props(p.fn, false)) {
                if (!(pp.flags & (CPF_ReturnParm | CPF_OutParm))) continue;
                json v = { {"type", pp.type} };
                const uintptr_t b = (uintptr_t)p.buf.data();
                if (pp.type == "ObjectProperty") { const uintptr_t ob = get<uintptr_t>(b, pp); v["value"] = ob ? full_name(ob) : ""; v["addr"] = hexstr(ob); }
                else if (pp.type == "StrProperty") v["value"] = fstring(b + pp.offset);
                else if (pp.type == "TextProperty") v["value"] = ftext(b + pp.offset);
                else if (pp.type == "NameProperty") v["value"] = name_str(get<FName>(b, pp));
                else if (pp.type == "ArrayProperty") {
                    auto arr = array_of(b, pp); v["num"] = arr.num;
                    Prop in = array_inner(pp);   // names / strings / numbers / small structs come back in full (max 5000)
                    if (in && arr.data && arr.num > 0) {
                        json items = json::array();
                        for (int32_t i = 0; i < arr.num && i < 5000; ++i) {
                            const uintptr_t e = (uintptr_t)arr.data + (uintptr_t)i * in.size;
                            if (in.type == "NameProperty") { FName n{}; mread(e, &n, 8); items.push_back(name_str(n)); }
                            else if (in.type == "StrProperty") items.push_back(fstring(e));
                            else if (in.type == "ObjectProperty") { uintptr_t o = 0; mread(e, &o, 8); items.push_back(o ? full_name(o) : ""); }
                            else if (in.type == "StructProperty" && in.owner_struct) items.push_back(struct_value(e, in.owner_struct, 1));
                            else if (in.type == "GbxDefPtrProperty" || in.type == "GameDataHandleProperty") items.push_back(def_to_string((const uint8_t*)e, in.type));
                            else { Prop t = in; t.offset = 0; items.push_back(get_num(e, t)); }
                        }
                        v["items"] = items;
                    }
                }
                else if (pp.type == "GbxDefPtrProperty" || pp.type == "GameDataHandleProperty") {
                    v["bytes"] = to_hex(p.at(pp), std::min(pp.size, 0x18)); v["value"] = def_to_string(p.at(pp), pp.type); }
                else if (pp.type == "StructProperty") {
                    v["bytes"] = to_hex(p.at(pp), std::min(pp.size, 64));
                    if (pp.owner_struct) v["value"] = struct_value(b + pp.offset, pp.owner_struct, 1);
                }
                else v["value"] = get_num(b, pp);
                r[pp.name] = v;
            }
            return ok(r);
        }
    }
}

} // namespace ue
