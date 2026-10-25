// Halo Map Porter - reading Halo 1 Unlimited's perf log (a CSV a map session, perf\<map>_<date>.csv beside the XBE):
// what limits a map's frame rate on a real Xbox (the processor's game ticks or its drawing, or the graphics chip),
// the biggest costs of each, what's on screen when it's slow, and which of the app's options go after them. Its
// rows (oxbox_perf.c in Halo 1 Unlimited): P, a second's summary; T, a camera tour's view (the same columns, over the
// view's frames); S, a slow frame; M, memory once at map load. Times are in ms a frame.
#pragma once
#include <filesystem>
#include <map>
#include <string>
#include <vector>

namespace perflog {

struct Row {
    char kind = 0;
    std::vector<std::string> v;   // the fields, as in the log's "# fields" line
};

struct Log {
    std::filesystem::path file;
    std::string map, build, started;
    std::vector<std::string> fields;
    std::map<std::string, int> column;
    std::vector<Row> rows;
    // A field's number (NaN when the row has it empty or the log has no such column).
    double num(const Row &r, const char *field) const;
    std::string text(const Row &r, const char *field) const;
    bool has(const char *field) const { return column.count(field) > 0; }
};

// False (with why in `err`) when the file isn't a perf log the app can read.
bool load(const std::filesystem::path &csv, Log &out, std::string &err);

// The report: plain text, lines ending in '\n'.
std::string report(const Log &log);

// Several logs side by side (the same map before and after a change, or one build's logs): their frame rates, what
// limited them, and their camera tour views stop for stop.
std::string compare(const std::vector<Log> &logs);

}   // namespace perflog
