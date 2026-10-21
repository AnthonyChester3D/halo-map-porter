// Halo Map Porter - Xbox map card editor window (Windows).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <functional>

#include "mapcard.hpp"

namespace mapcard {
// Decode a PNG/JPEG/BMP/GIF/TIFF with Windows Imaging Component; scales down so neither side exceeds max_side.
bool load_image(const fs::path &file, Image &out, int max_side = 2048, bool keep_alpha = false);
// The card's picture, decoded from its folder (false when it has none or it can't be read).
bool card_image(const fs::path &dir, const Card &c, Image &out);
// Modal editor for the card stored in `dir`. `map_file` is only shown ("putput.map"). Saves on close.
// `where` (optional) returns a line about where and when the files get written, for the card as it is now.
void edit_card(HWND owner, const fs::path &dir, const std::string &default_name, const std::wstring &map_file,
               const std::wstring &title, std::function<std::wstring(const Card &)> where = nullptr);
}
