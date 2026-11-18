// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace wb::serial {
struct Val { enum Kind { Int, Bit, Soft } kind = Int; uint64_t v = 0; int width = 0; };   // width: varint nibbles / varbit bits as read
struct Tok {
    enum Kind { Sep, Soft, Value, Part, Str } kind = Sep;
    Val val;                                   // Value
    Val idx; enum Sub { Plain, One, List } sub = Plain; Val one; std::vector<Val> list;   // Part
    std::string str;                           // Str
};
struct Item { std::vector<Tok> toks; int nbytes = 0; };

bool decode(const std::string& serial, Item& it, std::string* err = nullptr);
std::string encode(const Item& it);
std::string text(const Item& it);              // "20 , 0 , 1 , 50 | 2 , 1771 | | {96} {62} ..."
int type_index(const Item& it);
int level_tok(const Item& it);                 // token index of the level, only when the header is "<type> , 0 , 1 , <level> |"; else -1
int level(const Item& it);                     // -1 when the item has no level field (other header layout)
bool set_level(Item& it, int lv);              // false when the item has no level field
int first_part(const Item& it);                // token index of the first part token (-1 = none)
int comp_part(const Item& it);                 // token index of the comp (rarity) part: the first part, when it is a plain
                                               // part and no string token comes before it (a string there IS the comp); else -1
bool round_trips(const std::string& serial);   // decodes and encodes back to exactly the same text
} // namespace wb::serial
