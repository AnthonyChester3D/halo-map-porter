// Halo Map Porter - a small model viewer (Windows child window) for the weapons & vehicles window: drag to turn
// the model, mouse wheel to zoom, double-click to put it back. Drawn in software (render.hpp).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

#include "core.hpp"
#include "render.hpp"

namespace modelview {

HWND create(HWND parent, int x, int y, int w, int h, int id);
// Shows `m` (kept by the caller while shown), or `empty_text` when it's null. A new model starts at the usual view,
// unless `keep_view` (another detail level of the same model: seen the same way).
void show(HWND view, const porter::ModelPreview *m, const std::wstring &empty_text = L"", bool keep_view = false);
// Draw it placed and sized as `frame` would be (another version of the same model; null: as itself).
void set_frame(HWND view, const porter::ModelPreview *frame);
// How it's turned and zoomed, to put back after the model is shown again (render.hpp).
render::View get_view(HWND view);
void set_view(HWND view, const render::View &v);
// Draw the triangles' edges over the model.
void show_triangles(HWND view, bool on);

}   // namespace modelview
