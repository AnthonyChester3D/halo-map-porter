// Halo Map Porter - Invader's invader-edit, invader-dependency and invader-refactor, done in the app's own process.
//
// Starting a tool for every value the app reads or changes in a tag (hundreds per conversion) costs far more than the
// work itself, most of all on Windows. These read and write any tag kind the way Invader's tools do: the same keys,
// the same printed values, and a saved tag byte for byte as invader-edit saves it (tag_defs.inc, from Invader's own
// tag definitions). Anything they don't do exactly the same way (an option the app doesn't use, a tag or value Invader
// would refuse with an error) returns false, and the caller runs the tool itself.
#pragma once
#include <string>
#include <vector>

namespace tagedit {

// `args` as for the tool, without the program: {"-t", dir, tag, "-S", key, value, ...}. On true, `output` is what the
// tool prints (stdout and stderr, '\n' line ends) and `code` its exit code.
bool run_edit(const std::vector<std::string> &args, std::string &output, int &code);
bool run_dependency(const std::vector<std::string> &args, std::string &output, int &code);
bool run_refactor(const std::vector<std::string> &args, std::string &output, int &code);   // -M no-move only

// For tests: read a tag and save it again as invader-edit would (false if it can't be read the same way).
bool resave(const std::string &in, const std::string &out);

// For tests: how many calls were done in process, and how many fell back to the tool.
struct Stats { long handled = 0, fallback = 0; };
Stats stats();

}   // namespace tagedit
