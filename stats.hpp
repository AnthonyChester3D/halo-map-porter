// Halo Map Porter - map stats: which weapons, vehicles, bipeds, skies and weather the converted maps use.
//
// Each Custom Edition or MCC map the app takes apart leaves a small record of those tags (record): each one's path,
// whether it's Bungie's as made, Bungie's edited (the same path, other content) or the map's own, and a fingerprint of
// its content, so one tag reused under the same path in many maps counts once per map, with its versions told apart.
// The report (report_html) adds them up over every map recorded: the most used of each kind, the maps' own first.
// This part is portable; the batch window's "Map stats" opens the report.
#pragma once
#include <filesystem>
#include <string>

namespace stats {
namespace fs = std::filesystem;

// The map `input`'s tags, extracted in `tags`, recorded in `dir` (replacing what this map left before). `stock_tags`:
// Bungie's Custom Edition tags, extracted (empty: none is marked Bungie's).
void record(const fs::path &dir, const fs::path &input, const fs::path &tags, const fs::path &stock_tags);

// The report over every map recorded in `dir`, as an HTML page; `top`: how many of each kind. `maps`: set to how many
// maps were counted.
std::string report_html(const fs::path &dir, int top, int &maps);
}   // namespace stats
