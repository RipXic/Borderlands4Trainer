// Borderlands 4 Trainer - Copyright (c) 2026 RipXic. Source-available, see LICENSE.
// Borderlands 4: implemented in bl4.cpp. Runs on the game thread (main tick + request queue).
#pragma once
#include "common.h"

namespace dk {

void init(const std::string& data_dir, const std::string& panel_spec);   // once: loads state, starts the game-thread tick
bool native_mode();
void post(const json& requests);          // [{seq, op: set|click|call|all_off, ...}] - processed on the game thread
json state();                             // { seq_done, results, values, readouts, keeper, player, panel_log }
uint64_t state_version();                 // changes whenever state() would return something new

json cmd(const std::string& name, const json& a);

} // namespace dk
