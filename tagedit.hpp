// Halo Map Porter - Invader's invader-edit, invader-dependency and invader-refactor, done in the app's own process.
//
// Starting a tool for every value the app reads or changes in a tag (hundreds per conversion) costs far more than the
// work itself, most of all on Windows. These read and write any tag kind the way Invader's tools do: the same keys,
// the same printed values, and a saved tag byte for byte as invader-edit saves it (tag_defs.inc, from Invader's own
// tag definitions). Anything they don't do exactly the same way (an option the app doesn't use, a tag or value Invader
// would refuse with an error) returns false, and the caller runs the tool itself.
#pragma once
#include <string>
#include <utility>
#include <vector>

namespace tagedit {

// `args` as for the tool, without the program: {"-t", dir, tag, "-S", key, value, ...}. On true, `output` is what the
// tool prints (stdout and stderr, '\n' line ends) and `code` its exit code.
bool run_edit(const std::vector<std::string> &args, std::string &output, int &code);
bool run_dependency(const std::vector<std::string> &args, std::string &output, int &code);
bool run_refactor(const std::vector<std::string> &args, std::string &output, int &code);   // -M no-move only

// For tests: read a tag and save it again as invader-edit would (false if it can't be read the same way).
bool resave(const std::string &in, const std::string &out);

// Every value of a tag, "block[3].field" -> its text as invader-edit prints it (bitfields as the names of the bits set,
// data as its size and crc32, a reference as "@" and its path.group). False if the tag can't be read. `fourcc`, when
// given, gets the tag's group.
bool flatten(const std::string &file, std::vector<std::pair<std::string, std::string>> &out, unsigned *fourcc = nullptr);

// Copies these fields from tag `from` to tag `to` (the same kind) and saves `to` as invader-edit would. A field is a
// name, or "block.field" through blocks (each element both tags have): "multiplayer_information.vehicles". A block
// field copies the whole block. False (and `why`) if a tag can't be read or written, or a field isn't there.
bool graft(const std::string &from, const std::string &to, const std::vector<std::string> &fields,
           std::string *why = nullptr);

// A number field of the tag's main struct ("autoaim_angle"): its offset in the struct and its bytes as a cache file
// stores them (each number little-endian). False if there's no such number field.
bool root_field(const std::string &file, const std::string &name, unsigned &offset, std::string &le);

// For tests: how many calls were done in process, and how many fell back to the tool.
struct Stats { long handled = 0, fallback = 0; };
Stats stats();

}   // namespace tagedit
