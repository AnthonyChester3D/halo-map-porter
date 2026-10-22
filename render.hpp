// Halo Map Porter - draws a ModelPreview (core.hpp) in software: textured, lit from the viewer's upper left, with
// depth, edges smoothed (drawn at twice the size, then halved). Portable, for the weapons & vehicles window and tests.
#pragma once
#include <cstdint>
#include <vector>

#include "core.hpp"

namespace render {

struct View {
    float yaw = 0.75f;     // turned around the model's up axis (radians)
    float pitch = 0.32f;   // looked at from above (radians)
    float zoom = 1.0f;     // 1: the whole model fits
};

// Draws `m` into `px`: w x h pixels, top row first, each 0x00RRGGBB (a Windows 32-bit DIB's layout).
void draw(const porter::ModelPreview &m, const View &v, int w, int h, std::vector<uint32_t> &px);

}   // namespace render
