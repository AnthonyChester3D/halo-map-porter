// Halo Map Porter - the Convert tab's "Batch...": a list of maps (a playlist) converted for the Xbox one after
// another, each to where it was saved before, with the Xbox tab's options as they are and each map's own picks.
// Windows only. The list is kept between runs of the app.
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <filesystem>
#include <functional>
#include <string>
#include <vector>

namespace batchui {

struct Item {
    std::wstring in, out;   // the map converted, and where the Xbox map goes
    bool on = true;         // ticked: converted by "Convert ticked"
    std::wstring status;    // what happened the last time it was converted here (kept with the list)
    std::wstring size;      // the Xbox map's size then ("32.95 MB", uncompressed: what the Xbox's limit counts)
};

struct Hooks {
    // The maps converted for the Xbox before (from the app's records): their inputs and outputs.
    std::function<std::vector<Item>()> find;
    // The Convert tab's map and "Save as", to add (false if there's none).
    std::function<bool(Item &)> current;
    // "Custom Edition", "MCC"... for the list.
    std::function<std::wstring(const std::wstring &in)> game_of;
    // Converts these (the ticked ones, by their index in the list) in a worker thread; it reports with progress()
    // and done(). False (after telling the user) if it can't start.
    // `at_once`: how many maps are converted at the same time (each in its own thread and work folder).
    std::function<bool(const std::vector<Item> &items, const std::vector<int> &which, int at_once)> start;
    // Asked to stop after the map being converted.
    std::function<void()> stop;
    // Where a map picked with "Browse..." goes (its earlier output, else beside the others); "" if it can't tell.
    std::function<std::wstring(const std::wstring &in)> default_out;
    // The map card you made for a map ("Picture, description"; "" none: what a conversion fills in itself doesn't
    // count), and the card editor for it (modal).
    std::function<std::wstring(const Item &)> card_of;
    std::function<void(HWND owner, const Item &)> open_card;
    // A map put on the Convert tab (its map and where it's saved): double-click, or right-click "Use on the Convert
    // tab". The window closes after it (not while converting).
    std::function<void(const Item &)> use;
    // "Maps at once" as the window opens (1 to kMaxAtOnce), and told when it's changed (to keep it).
    int at_once = 1;
    std::function<void(int)> at_once_changed;
};
const int kMaxAtOnce = 8;

// The window (modal); the list is read from and saved to `list_file`.
void open(HWND owner, const std::filesystem::path &list_file, Hooks hooks);
// From the worker (any thread): item `index`'s status (and its map's size, when there's one).
void progress(int index, const std::wstring &status, const std::wstring &size = L"\x01");
// From the worker (any thread): the run is over.
void done(const std::wstring &summary);

}   // namespace batchui
