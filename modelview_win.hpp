// Halo Map Porter - a small model viewer (Windows child window) for the weapons & vehicles window: drag to turn
// the model, mouse wheel to zoom, double-click to put it back. Drawn in software (render.hpp).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <string>

#include "core.hpp"

namespace modelview {

HWND create(HWND parent, int x, int y, int w, int h, int id);
// Shows `m` (kept by the caller while shown), or `empty_text` when it's null. A new model starts at the usual view,
// unless `keep_view` (another detail level of the same model: seen the same way).
void show(HWND view, const porter::ModelPreview *m, const std::wstring &empty_text = L"", bool keep_view = false);
// Draw the triangles' edges over the model.
void show_triangles(HWND view, bool on);

}   // namespace modelview
