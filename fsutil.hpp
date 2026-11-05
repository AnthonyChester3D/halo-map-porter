// Halo Map Porter - file helpers.
#pragma once
#include <filesystem>
#include <system_error>

// A file copied over another (`to` replaced). Not std::filesystem::copy_file's overwrite_existing: MinGW's takes any two
// files on the same drive for the same file (no file ids on Windows) and copies nothing, saying "file exists".
inline bool copy_over(const std::filesystem::path &from, const std::filesystem::path &to, std::error_code &ec) {
    if (from.lexically_normal() == to.lexically_normal()) { ec.clear(); return true; }   // (itself: nothing to do)
    std::error_code e;
    if (!std::filesystem::exists(from, e)) return std::filesystem::copy_file(from, to, ec);   // (fails: `to` stays)
    if (std::filesystem::exists(to, e)) std::filesystem::remove(to, e);
    return std::filesystem::copy_file(from, to, ec);
}
