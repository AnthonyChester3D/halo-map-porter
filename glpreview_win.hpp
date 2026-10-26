// Halo Map Porter - the level shaders window's picture on the graphics card (OpenGL 2, Windows): the same picture as
// lshade::draw (levelshaders.hpp), fast enough to fly through. The window falls back to lshade::draw when the
// machine has no OpenGL 2 (create() returns nothing).
#pragma once
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include <array>
#include <memory>
#include <vector>

#include "levelshaders.hpp"

namespace glpreview {

struct Frame {
    lshade::Camera camera;
    std::vector<char> made, converted;   // per shader: its reflection drawn as made / as converted
    bool what_it_loses = false;          // the converted picture dimmed, what dropping reflections loses lit up
    int highlight = -1;                  // a shader to tint
    bool portals = false;                // portals outlined, rooms tinted
    bool fog_on = false;                 // the level fogged (and cut off past opaque fog), as the engine does
    lshade::Fog fog;
    bool objects = false;                // the objects in the level (lshade::Preview::objects) drawn
    // The showcase's shot being edited: its camera path (a line), its keys (points), what it looks at, and the boxes
    // around the map's placed objects (the camera keeps clear of them), outlined.
    std::vector<std::array<float, 3>> path, keys;
    int key = -1;                        // (the picked one, in `keys`)
    bool look = false;
    std::array<float, 3> look_at{};
    const std::vector<lshade::Box> *boxes = nullptr;
};

class Renderer {
public:
    // OpenGL 2 on `view` (a window of its own with the CS_OWNDC class style): nothing when it can't be had.
    static std::unique_ptr<Renderer> create(HWND view);
    ~Renderer();
    // The level to draw (uploaded to the card; nullptr: none).
    void set_scene(const lshade::Preview *p);
    // Draws a frame to the whole view and shows it.
    void draw(const Frame &f);

private:
    Renderer() = default;
    struct Impl;
    std::unique_ptr<Impl> d;
};

}   // namespace glpreview
