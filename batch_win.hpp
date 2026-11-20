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
    // "Multiplayer", "Campaign" or "UI", from the map's header ("" if it can't be read).
    std::function<std::wstring(const std::wstring &in)> kind_of;
    // Converts these (the ticked ones, by their index in the list) in a worker thread; it reports with progress()
    // and done(). False (after telling the user) if it can't start.
    // `at_once`: how many maps are converted at the same time (each in its own thread and work folder).
    std::function<bool(const std::vector<Item> &items, const std::vector<int> &which, int at_once)> start;
    // "Showcase & bots only": for these (the ticked ones), the showcase flythrough and bot file made from the map and
    // written beside its Xbox map, which isn't built again or touched, in a worker thread (progress() and done(), as
    // start's). False if it can't start.
    std::function<bool(const std::vector<Item> &items, const std::vector<int> &which)> extras;
    // Asked to stop after the map being converted.
    std::function<void()> stop;
    // Where a map picked with "Browse..." goes (its earlier output, else beside the others); "" if it can't tell.
    std::function<std::wstring(const std::wstring &in)> default_out;
    // The map card you made for a map ("Picture, description"; "" none: what a conversion fills in itself doesn't
    // count), and the card editor for it (modal).
    std::function<std::wstring(const Item &)> card_of;
    // Each map's pack, the batch's way ("" for none): an MCC Workshop item's maps when it has two or more, or maps
    // whose map cards share a folder (a campaign's chapters). One per item, in order.
    std::function<std::vector<std::wstring>(const std::vector<Item> &)> packs;
    std::function<void(HWND owner, const Item &)> open_card;
    // A map put on the Convert tab (its map and where it's saved): double-click, or right-click "Use on the Convert
    // tab". The window closes after it (not while converting).
    std::function<void(const Item &)> use;
    // "Maps at once" as the window opens (1 to kMaxAtOnce), and told when it's changed (to keep it).
    int at_once = 1;
    std::function<void(int)> at_once_changed;
    // Where the batch's maps go (the Build tab's Xbox folder) and its four map pack folders' names: linked pairs
    // (the Xbox maps), the Custom Edition maps they're linked to, the MCC maps they're linked to (laid out as OpenCE's
    // mcc_maps folder), and maps converted but not linked. Shown in the
    // window; told when one's changed (and before converting), to keep them. Empty folder: each map where it's saved.
    std::wstring root, crossplay, crossplay_ce, crossplay_mcc, xbox;
    std::function<void(const std::wstring &root, const std::wstring &crossplay, const std::wstring &crossplay_ce,
                       const std::wstring &crossplay_mcc, const std::wstring &xbox)> layout_changed;
    // A folder picked with the window's "Browse..." for it ("" cancelled).
    std::function<std::wstring(HWND owner, const std::wstring &start)> pick_folder;
    // "MCC Workshop...": the Halo 1 maps (Custom Edition or MCC, by their headers) in MCC's Steam Workshop folder, found
    // or picked; `note` says what was left out (other games' maps) or why none ("" and an empty list: cancelled).
    std::function<std::vector<std::wstring>(HWND owner, std::wstring &note)> workshop;
    // "Map stats": the report on the weapons, vehicles, bipeds, skies and weather of every map converted, opened.
    std::function<void(HWND owner)> stats;
};
const int kMaxAtOnce = 8;

// The window (modal); the list is read from and saved to `list_file`.
void open(HWND owner, const std::filesystem::path &list_file, Hooks hooks);
// From the worker (any thread): item `index`'s status (and its map's size, and where it went, when there's one).
void progress(int index, const std::wstring &status, const std::wstring &size = L"\x01", const std::wstring &out = L"\x01");
// From the worker (any thread): the run is over.
void done(const std::wstring &summary);

}   // namespace batchui
