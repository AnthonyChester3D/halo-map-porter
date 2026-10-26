// Halo Map Porter - the level shaders window's picture on the graphics card (see glpreview_win.hpp).
#include "glpreview_win.hpp"

#include <GL/gl.h>
#include <GL/glext.h>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdlib>
#include <map>
#include <string>
#include <tuple>

namespace glpreview {

namespace {

// OpenGL 2's functions (Windows' opengl32 has 1.1's only; the driver has the rest).
#define GLFUNCS(X)                                                                                                  \
    X(PFNGLCREATESHADERPROC, glCreateShader) X(PFNGLSHADERSOURCEPROC, glShaderSource)                              \
    X(PFNGLCOMPILESHADERPROC, glCompileShader) X(PFNGLGETSHADERIVPROC, glGetShaderiv)                              \
    X(PFNGLCREATEPROGRAMPROC, glCreateProgram) X(PFNGLATTACHSHADERPROC, glAttachShader)                            \
    X(PFNGLLINKPROGRAMPROC, glLinkProgram) X(PFNGLGETPROGRAMIVPROC, glGetProgramiv)                                \
    X(PFNGLUSEPROGRAMPROC, glUseProgram) X(PFNGLGETUNIFORMLOCATIONPROC, glGetUniformLocation)                      \
    X(PFNGLUNIFORM1IPROC, glUniform1i) X(PFNGLUNIFORM1FPROC, glUniform1f) X(PFNGLUNIFORM3FPROC, glUniform3f)       \
    X(PFNGLDELETESHADERPROC, glDeleteShader) X(PFNGLDELETEPROGRAMPROC, glDeleteProgram)                            \
    X(PFNGLGENBUFFERSPROC, glGenBuffers) X(PFNGLBINDBUFFERPROC, glBindBuffer) X(PFNGLBUFFERDATAPROC, glBufferData)  \
    X(PFNGLDELETEBUFFERSPROC, glDeleteBuffers) X(PFNGLACTIVETEXTUREPROC, glActiveTexture)                          \
    X(PFNGLCLIENTACTIVETEXTUREPROC, glClientActiveTexture)
#define DECLARE(type, name) type name = nullptr;
struct GL { GLFUNCS(DECLARE) };
#undef DECLARE

const char *kVertex = R"(#version 110
varying vec3 v_pos;
varying vec3 v_n;
varying vec2 v_uv;
varying vec2 v_luv;
varying float v_room;
varying float v_depth;
void main() {
    v_pos = gl_Vertex.xyz;
    v_depth = -(gl_ModelViewMatrix * gl_Vertex).z;
    v_n = gl_Normal;
    v_uv = gl_MultiTexCoord0.xy;
    v_luv = gl_MultiTexCoord1.xy;
    v_room = gl_MultiTexCoord2.x;
    gl_Position = ftransform();
}
)";

// As lshade::draw: base x 2 x detail (at most 1) x lightmap x 2, plus the reflection (the cube map in the reflected
// direction, between its parallel and perpendicular colour by how straight on it's seen, masked by the base map's
// alpha and, with a lightmap brightness scale under 1, dimmed where the lightmap is dark). Alpha-tested surfaces are
// cut by their bump map's alpha.
const char *kFragment = R"(#version 110
uniform sampler2D u_base;
uniform sampler2D u_detail;
uniform sampler2D u_lm;
uniform samplerCube u_cube;
uniform sampler2D u_cut;
uniform float u_has_base, u_has_detail, u_has_lm, u_has_cube, u_has_cut, u_cut_scale;
uniform float u_detail_scale, u_mask_alpha, u_lm_scale;
uniform float u_made, u_converted, u_loses, u_highlight, u_rooms, u_see_through;
uniform vec3 u_perp, u_para, u_eye;
uniform vec3 u_fog_color;
uniform float u_fog_density, u_fog_start, u_fog_opaque;
varying vec3 v_pos;
varying vec3 v_n;
varying vec2 v_uv;
varying vec2 v_luv;
varying float v_room;
varying float v_depth;
vec3 fogged(vec3 c) {
    if (u_fog_density <= 0.0) return c;
    float f = u_fog_density * clamp((v_depth - u_fog_start) / max(0.001, u_fog_opaque - u_fog_start), 0.0, 1.0);
    return mix(min(c, 1.0), u_fog_color, f);
}
vec3 room_colour(float r) {
    float h = fract(r * 0.618033988749895) * 6.0;
    float s = 0.75, v = 0.95, i = floor(h), f = h - i;
    float p = v * (1.0 - s), q = v * (1.0 - s * f), t = v * (1.0 - s * (1.0 - f));
    if (i < 0.5) return vec3(v, t, p);
    if (i < 1.5) return vec3(q, v, p);
    if (i < 2.5) return vec3(p, v, t);
    if (i < 3.5) return vec3(p, q, v);
    if (i < 4.5) return vec3(t, p, v);
    return vec3(v, p, q);
}
void main() {
    // (alpha-tested: drawn only where its cut-out, the bump map's alpha, is over half, as the Xbox's 0x7F "greater")
    if (u_has_cut > 0.5 && texture2D(u_cut, v_uv * u_cut_scale).a <= 0.498) discard;
    vec4 b = u_has_base > 0.5 ? texture2D(u_base, v_uv) : vec4(0.6, 0.6, 0.6, 1.0);
    if (u_see_through > 0.5) {
        gl_FragColor = vec4(fogged(u_has_base > 0.5 ? b.rgb : vec3(0.55, 0.65, 0.75)), 0.35);
        return;
    }
    vec4 d = u_has_detail > 0.5 ? texture2D(u_detail, v_uv * u_detail_scale) : vec4(0.5);
    vec4 l = u_has_lm > 0.5 ? texture2D(u_lm, v_luv) : vec4(0.5);
    vec3 col = min(vec3(1.0), b.rgb * 2.0 * d.rgb) * l.rgb * 2.0;
    vec3 r = vec3(0.0);
    if (u_has_cube > 0.5) {
        vec3 n = normalize(v_n);
        vec3 dir = normalize(v_pos - u_eye);
        float facing = abs(dot(n, dir));
        vec3 rd = dir - n * (2.0 * dot(dir, n));
        float mask = u_mask_alpha > 0.5 ? b.a : 1.0;
        if (u_lm_scale < 1.0) mask *= u_lm_scale + (1.0 - u_lm_scale) * min(1.0, dot(l.rgb, vec3(0.3, 0.59, 0.11)) * 2.0);
        r = textureCube(u_cube, rd).rgb * (u_para + (u_perp - u_para) * facing) * mask;
    }
    vec3 o;
    if (u_loses > 0.5) {
        vec3 made = fogged(col + r * u_made), conv = fogged(col + r * u_converted);
        o = conv * 0.3 + abs(made - conv) * 4.0;
    } else {
        o = fogged(col + r * u_converted);
        if (u_highlight > 0.5) o = vec3(o.r * 0.5 + 0.5, o.g * 0.5, o.b * 0.5 + 0.45);
        if (u_rooms > 0.5 && v_room >= 0.0) o = min(o, 1.0) * 0.6 + room_colour(v_room) * 0.4;
    }
    gl_FragColor = vec4(min(o, 1.0), 1.0);
}
)";

struct Vtx { float p[3], n[3], uv[2], luv[2], room; };
struct Group { int material = -1, lightmap = -1; bool see_through = false; GLint first = 0; GLsizei count = 0; };

}   // namespace

struct Renderer::Impl {
    HWND wnd = nullptr;
    HDC dc = nullptr;
    HGLRC rc = nullptr;
    GL gl;
    GLuint program = 0, vbo = 0;
    std::map<std::string, GLint> loc;
    const lshade::Preview *scene = nullptr;
    std::vector<Group> groups;
    std::vector<GLuint> tex2d, cube;   // per preview texture (0: none)
    size_t vertices = 0;

    GLint u(const char *name) {
        auto it = loc.find(name);
        if (it != loc.end()) return it->second;
        return loc[name] = gl.glGetUniformLocation(program, name);
    }
    void clear_scene() {
        if (!tex2d.empty()) glDeleteTextures((GLsizei)tex2d.size(), tex2d.data());
        if (!cube.empty()) glDeleteTextures((GLsizei)cube.size(), cube.data());
        tex2d.clear();
        cube.clear();
        groups.clear();
        if (vbo) gl.glDeleteBuffers(1, &vbo);
        vbo = 0;
        vertices = 0;
        scene = nullptr;
    }
};

std::unique_ptr<Renderer> Renderer::create(HWND view) {
    auto d = std::make_unique<Impl>();
    d->wnd = view;
    d->dc = GetDC(view);
    if (!d->dc) return nullptr;
    PIXELFORMATDESCRIPTOR pfd{};
    pfd.nSize = sizeof(pfd);
    pfd.nVersion = 1;
    pfd.dwFlags = PFD_DRAW_TO_WINDOW | PFD_SUPPORT_OPENGL | PFD_DOUBLEBUFFER;
    pfd.iPixelType = PFD_TYPE_RGBA;
    pfd.cColorBits = 32;
    pfd.cDepthBits = 24;
    pfd.iLayerType = PFD_MAIN_PLANE;
    int pf = ChoosePixelFormat(d->dc, &pfd);
    auto fail = [&]() -> std::unique_ptr<Renderer> {
        if (d->rc) { wglMakeCurrent(nullptr, nullptr); wglDeleteContext(d->rc); }
        ReleaseDC(view, d->dc);
        return nullptr;
    };
    if (!pf || !SetPixelFormat(d->dc, pf, &pfd)) return fail();
    d->rc = wglCreateContext(d->dc);
    if (!d->rc || !wglMakeCurrent(d->dc, d->rc)) return fail();
    // Frames at the screen's refresh (smooth, and no faster than it shows them).
    typedef BOOL(WINAPI * SwapInterval)(int);
    if (auto swap = (SwapInterval)(void *)wglGetProcAddress("wglSwapIntervalEXT")) swap(1);
    const char *version = (const char *)glGetString(GL_VERSION);
    if (!version || std::atoi(version) < 2) return fail();
#define LOAD(type, name) d->gl.name = (type)(void *)wglGetProcAddress(#name); if (!d->gl.name) return fail();
    GLFUNCS(LOAD)
#undef LOAD
    auto compile = [&](GLenum kind, const char *src) -> GLuint {
        GLuint s = d->gl.glCreateShader(kind);
        d->gl.glShaderSource(s, 1, &src, nullptr);
        d->gl.glCompileShader(s);
        GLint ok = 0;
        d->gl.glGetShaderiv(s, GL_COMPILE_STATUS, &ok);
        if (!ok) { d->gl.glDeleteShader(s); return 0; }
        return s;
    };
    GLuint vs = compile(GL_VERTEX_SHADER, kVertex), fs = compile(GL_FRAGMENT_SHADER, kFragment);
    if (!vs || !fs) return fail();
    d->program = d->gl.glCreateProgram();
    d->gl.glAttachShader(d->program, vs);
    d->gl.glAttachShader(d->program, fs);
    d->gl.glLinkProgram(d->program);
    d->gl.glDeleteShader(vs);
    d->gl.glDeleteShader(fs);
    GLint linked = 0;
    d->gl.glGetProgramiv(d->program, GL_LINK_STATUS, &linked);
    if (!linked) return fail();
    std::unique_ptr<Renderer> r(new Renderer());
    r->d = std::move(d);
    return r;
}

Renderer::~Renderer() {
    if (!d) return;
    wglMakeCurrent(d->dc, d->rc);
    d->clear_scene();
    if (d->program) d->gl.glDeleteProgram(d->program);
    wglMakeCurrent(nullptr, nullptr);
    wglDeleteContext(d->rc);
    ReleaseDC(d->wnd, d->dc);
}

void Renderer::set_scene(const lshade::Preview *p) {
    wglMakeCurrent(d->dc, d->rc);
    d->clear_scene();
    if (!p) return;
    d->scene = p;
    GL &gl = d->gl;
    // Textures: which are cube maps, which flat.
    std::vector<char> is_cube(p->textures.size(), 0), is_flat(p->textures.size(), 0);
    for (auto &m : p->materials) {
        if (m.cube >= 0 && (size_t)m.cube < is_cube.size()) is_cube[(size_t)m.cube] = 1;
        for (int t : {m.base, m.detail, m.cutout})
            if (t >= 0 && (size_t)t < is_flat.size()) is_flat[(size_t)t] = 1;
    }
    for (auto &t : p->triangles)
        if (t.lightmap >= 0 && (size_t)t.lightmap < is_flat.size()) is_flat[(size_t)t.lightmap] = 1;
    d->tex2d.assign(p->textures.size(), 0);
    d->cube.assign(p->textures.size(), 0);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);
    for (size_t i = 0; i < p->textures.size(); i++) {
        const lshade::Texture &t = p->textures[i];
        if (t.w <= 0 || t.h <= 0 || t.rgba.size() < (size_t)t.w * t.h * 4) continue;
        if (is_flat[i]) {
            glGenTextures(1, &d->tex2d[i]);
            glBindTexture(GL_TEXTURE_2D, d->tex2d[i]);
            glTexParameteri(GL_TEXTURE_2D, GL_GENERATE_MIPMAP, GL_TRUE);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, t.w, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
        }
        if (is_cube[i] && t.w >= 6) {
            int fw = t.w / 6;
            glGenTextures(1, &d->cube[i]);
            glBindTexture(GL_TEXTURE_CUBE_MAP, d->cube[i]);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
            glTexParameteri(GL_TEXTURE_CUBE_MAP, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, t.w);
            for (int f = 0; f < 6; f++) {   // (the strip's faces are in GL's order: +x -x +y -y +z -z)
                glPixelStorei(GL_UNPACK_SKIP_PIXELS, f * fw);
                glTexImage2D(GL_TEXTURE_CUBE_MAP_POSITIVE_X + f, 0, GL_RGBA, fw, t.h, 0, GL_RGBA, GL_UNSIGNED_BYTE, t.rgba.data());
            }
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
            glPixelStorei(GL_UNPACK_SKIP_PIXELS, 0);
        }
    }
    // The triangles, by what they're drawn with (opaque first, see-through after).
    std::vector<size_t> order;
    for (size_t i = 0; i < p->triangles.size(); i++)
        if (p->triangles[i].material >= 0 && (size_t)p->triangles[i].material < p->materials.size()) order.push_back(i);
    auto key = [&](size_t i) {
        const lshade::Triangle &t = p->triangles[i];
        return std::make_tuple(p->materials[(size_t)t.material].see_through, t.material, t.lightmap);
    };
    std::stable_sort(order.begin(), order.end(), [&](size_t a, size_t b) { return key(a) < key(b); });
    std::vector<Vtx> v;
    v.reserve(order.size() * 3);
    for (size_t i : order) {
        const lshade::Triangle &t = p->triangles[i];
        bool st = p->materials[(size_t)t.material].see_through;
        if (d->groups.empty() || d->groups.back().material != t.material || d->groups.back().lightmap != t.lightmap) {
            Group g;
            g.material = t.material;
            g.lightmap = t.lightmap;
            g.see_through = st;
            g.first = (GLint)v.size();
            d->groups.push_back(g);
        }
        float room = i < p->room.size() ? (float)p->room[i] : -1.0f;
        for (int k = 0; k < 3; k++) {
            Vtx x;
            for (int c = 0; c < 3; c++) { x.p[c] = t.v[k].p[c]; x.n[c] = t.v[k].n[c]; }
            x.uv[0] = t.v[k].uv[0]; x.uv[1] = t.v[k].uv[1];
            x.luv[0] = t.v[k].luv[0]; x.luv[1] = t.v[k].luv[1];
            x.room = room;
            v.push_back(x);
        }
        d->groups.back().count += 3;
    }
    d->vertices = v.size();
    gl.glGenBuffers(1, &d->vbo);
    gl.glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
    gl.glBufferData(GL_ARRAY_BUFFER, (GLsizeiptr)(v.size() * sizeof(Vtx)), v.data(), GL_STATIC_DRAW);
    gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
}

void Renderer::draw(const Frame &f) {
    wglMakeCurrent(d->dc, d->rc);
    GL &gl = d->gl;
    RECT rc;
    GetClientRect(d->wnd, &rc);
    int w = std::max(1L, rc.right), h = std::max(1L, rc.bottom);
    glViewport(0, 0, w, h);
    glDisable(GL_DEPTH_TEST);
    glDepthMask(GL_TRUE);
    glClearDepth(1.0);
    glClear(GL_DEPTH_BUFFER_BIT | GL_COLOR_BUFFER_BIT);
    // The sky (as lshade::draw's).
    glMatrixMode(GL_PROJECTION);
    glLoadIdentity();
    glMatrixMode(GL_MODELVIEW);
    glLoadIdentity();
    glBegin(GL_QUADS);
    glColor3f(0.45f, 0.50f, 0.56f); glVertex2f(-1, -1); glVertex2f(1, -1);
    glColor3f(0.30f, 0.36f, 0.46f); glVertex2f(1, 1); glVertex2f(-1, 1);
    glEnd();
    const lshade::Preview *p = d->scene;
    if (p && d->vbo) {
        // The camera, as lshade::draw's: 70 degrees across.
        const bool fog_on = f.fog_on && f.fog.any();
        const double tx = std::tan(35.0 * 3.14159265358979 / 180), ty = tx * h / w, n = 0.02,
                     far_ = fog_on && f.fog.cuts() ? std::max(0.1, (double)f.fog.opaque) : 5000;   // (opaque fog: nothing past it)
        glMatrixMode(GL_PROJECTION);
        glLoadIdentity();
        glFrustum(-tx * n, tx * n, -ty * n, ty * n, n, far_);
        const auto &c = f.camera;
        double fw[3] = {std::cos(c.yaw) * std::cos(c.pitch), std::sin(c.yaw) * std::cos(c.pitch), std::sin(c.pitch)};
        double rt[3] = {fw[1], -fw[0], 0};   // forward x up(0,0,1)
        double rl = std::sqrt(rt[0] * rt[0] + rt[1] * rt[1]);
        if (rl < 1e-9) { rt[0] = 1; rt[1] = 0; rl = 1; }
        rt[0] /= rl; rt[1] /= rl;
        double up[3] = {rt[1] * fw[2] - rt[2] * fw[1], rt[2] * fw[0] - rt[0] * fw[2], rt[0] * fw[1] - rt[1] * fw[0]};
        auto dt = [&](const double *a) { return a[0] * c.eye[0] + a[1] * c.eye[1] + a[2] * c.eye[2]; };
        double m[16] = {rt[0], up[0], -fw[0], 0, rt[1], up[1], -fw[1], 0, rt[2], up[2], -fw[2], 0, -dt(rt), -dt(up), dt(fw), 1};
        glMatrixMode(GL_MODELVIEW);
        glLoadMatrixd(m);

        glEnable(GL_DEPTH_TEST);
        glDepthFunc(GL_LESS);
        gl.glBindBuffer(GL_ARRAY_BUFFER, d->vbo);
        glEnableClientState(GL_VERTEX_ARRAY);
        glVertexPointer(3, GL_FLOAT, sizeof(Vtx), (void *)offsetof(Vtx, p));
        glEnableClientState(GL_NORMAL_ARRAY);
        glNormalPointer(GL_FLOAT, sizeof(Vtx), (void *)offsetof(Vtx, n));
        const size_t tc_offsets[3] = {offsetof(Vtx, uv), offsetof(Vtx, luv), offsetof(Vtx, room)};
        const int tc_sizes[3] = {2, 2, 1};
        for (int k = 0; k < 3; k++) {
            gl.glClientActiveTexture(GL_TEXTURE0 + k);
            glEnableClientState(GL_TEXTURE_COORD_ARRAY);
            glTexCoordPointer(tc_sizes[k], GL_FLOAT, sizeof(Vtx), (void *)tc_offsets[k]);
        }
        gl.glUseProgram(d->program);
        gl.glUniform1i(d->u("u_base"), 0);
        gl.glUniform1i(d->u("u_detail"), 1);
        gl.glUniform1i(d->u("u_lm"), 2);
        gl.glUniform1i(d->u("u_cube"), 3);
        gl.glUniform1i(d->u("u_cut"), 4);
        gl.glUniform3f(d->u("u_eye"), (float)c.eye[0], (float)c.eye[1], (float)c.eye[2]);
        gl.glUniform1f(d->u("u_loses"), f.what_it_loses ? 1.0f : 0.0f);
        gl.glUniform1f(d->u("u_rooms"), f.portals && !f.what_it_loses ? 1.0f : 0.0f);
        gl.glUniform3f(d->u("u_fog_color"), f.fog.color[0], f.fog.color[1], f.fog.color[2]);
        gl.glUniform1f(d->u("u_fog_density"), fog_on ? f.fog.density : 0.0f);
        gl.glUniform1f(d->u("u_fog_start"), f.fog.start);
        gl.glUniform1f(d->u("u_fog_opaque"), f.fog.opaque);
        auto bind2d = [&](int unit, int tex, const char *has) {
            GLuint id = tex >= 0 && (size_t)tex < d->tex2d.size() ? d->tex2d[(size_t)tex] : 0;
            gl.glActiveTexture(GL_TEXTURE0 + unit);
            glBindTexture(GL_TEXTURE_2D, id);
            gl.glUniform1f(d->u(has), id ? 1.0f : 0.0f);
            return id;
        };
        for (int pass = 0; pass < 2; pass++) {   // opaque, then see-through (blended, not hiding what's behind)
            if (pass == 1) {
                if (f.what_it_loses) break;
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                glDisable(GL_CULL_FACE);
            } else {
                glDisable(GL_BLEND);
                glDepthMask(GL_TRUE);
                glEnable(GL_CULL_FACE);   // (Halo winds a surface's corners clockwise seen from the front)
                glFrontFace(GL_CW);
                glCullFace(GL_BACK);
            }
            for (auto &g : d->groups) {
                if (g.see_through != (pass == 1)) continue;
                const lshade::Material &mt = p->materials[(size_t)g.material];
                gl.glUniform1f(d->u("u_see_through"), mt.see_through ? 1.0f : 0.0f);
                bind2d(0, mt.base, "u_has_base");
                bind2d(1, mt.detail, "u_has_detail");
                bind2d(2, g.lightmap, "u_has_lm");
                bind2d(4, mt.cutout, "u_has_cut");
                gl.glUniform1f(d->u("u_cut_scale"), mt.cutout_scale);
                GLuint cube = mt.cube >= 0 && (size_t)mt.cube < d->cube.size() ? d->cube[(size_t)mt.cube] : 0;
                gl.glActiveTexture(GL_TEXTURE3);
                glBindTexture(GL_TEXTURE_CUBE_MAP, cube);
                gl.glUniform1f(d->u("u_has_cube"), cube && mt.shader >= 0 ? 1.0f : 0.0f);
                gl.glUniform1f(d->u("u_detail_scale"), mt.detail_scale);
                gl.glUniform1f(d->u("u_mask_alpha"), mt.mask_by_base_alpha ? 1.0f : 0.0f);
                gl.glUniform1f(d->u("u_lm_scale"), mt.lightmap_scale);
                gl.glUniform3f(d->u("u_perp"), mt.perpendicular[0], mt.perpendicular[1], mt.perpendicular[2]);
                gl.glUniform3f(d->u("u_para"), mt.parallel[0], mt.parallel[1], mt.parallel[2]);
                auto on = [&](const std::vector<char> &v) { return mt.shader < 0 || (size_t)mt.shader >= v.size() || v[(size_t)mt.shader]; };
                gl.glUniform1f(d->u("u_made"), on(f.made) ? 1.0f : 0.0f);
                gl.glUniform1f(d->u("u_converted"), on(f.converted) ? 1.0f : 0.0f);
                gl.glUniform1f(d->u("u_highlight"), f.highlight >= 0 && mt.shader == f.highlight ? 1.0f : 0.0f);
                glDrawArrays(GL_TRIANGLES, g.first, g.count);
            }
        }
        gl.glUseProgram(0);
        for (int k = 4; k >= 0; k--) {
            gl.glActiveTexture(GL_TEXTURE0 + k);
            glBindTexture(GL_TEXTURE_2D, 0);
            glBindTexture(GL_TEXTURE_CUBE_MAP, 0);
        }
        for (int k = 2; k >= 0; k--) {
            gl.glClientActiveTexture(GL_TEXTURE0 + k);
            glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        }
        glDisableClientState(GL_NORMAL_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        gl.glBindBuffer(GL_ARRAY_BUFFER, 0);
        glDisable(GL_CULL_FACE);

        // The showcase's shot: the boxes the camera keeps clear of (grey), its path (cyan) and keys (yellow), and what
        // it looks at (magenta), drawn over the level where nothing is in front of them.
        if (f.boxes || !f.path.empty() || !f.keys.empty()) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glDepthFunc(GL_LEQUAL);
            if (f.boxes) {
                glLineWidth(1.0f);
                glColor4f(0.85f, 0.85f, 0.85f, 0.55f);
                for (auto &b : *f.boxes) {
                    float q[8][3];
                    for (int k = 0; k < 8; k++) {
                        float sx = (k & 1) ? b.half[0] : -b.half[0], sy = (k & 2) ? b.half[1] : -b.half[1], sz = (k & 4) ? b.half[2] : -b.half[2];
                        q[k][0] = b.c[0] + sx * b.axis[0][0] + sy * b.axis[1][0];
                        q[k][1] = b.c[1] + sx * b.axis[0][1] + sy * b.axis[1][1];
                        q[k][2] = b.c[2] + sz;
                    }
                    static const int edge[12][2] = {{0, 1}, {1, 3}, {3, 2}, {2, 0}, {4, 5}, {5, 7}, {7, 6}, {6, 4}, {0, 4}, {1, 5}, {2, 6}, {3, 7}};
                    glBegin(GL_LINES);
                    for (auto &e : edge) { glVertex3fv(q[e[0]]); glVertex3fv(q[e[1]]); }
                    glEnd();
                }
            }
            glLineWidth(3.0f);
            glColor4f(0.2f, 0.95f, 1.0f, 0.9f);
            glBegin(GL_LINE_STRIP);
            for (auto &q : f.path) glVertex3f(q[0], q[1], q[2]);
            glEnd();
            glPointSize(8.0f);
            glColor4f(1.0f, 0.9f, 0.15f, 1.0f);
            glBegin(GL_POINTS);
            for (auto &q : f.keys) glVertex3f(q[0], q[1], q[2]);
            glEnd();
            if (f.look) {
                glColor4f(1.0f, 0.3f, 0.9f, 1.0f);
                glPointSize(12.0f);
                glBegin(GL_POINTS);
                glVertex3f(f.look_at[0], f.look_at[1], f.look_at[2]);
                glEnd();
            }
            glPointSize(1.0f);
            glLineWidth(1.0f);
            glDepthFunc(GL_LESS);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
        // The portals: a glow where you see through one, and its outline (as lshade::draw's).
        if (f.portals && !f.what_it_loses) {
            glEnable(GL_BLEND);
            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
            glDepthMask(GL_FALSE);
            glDepthFunc(GL_LESS);
            glColor4f(1.0f, 0.62f, 0.1f, 0.28f);
            for (auto &po : p->portals) {
                glBegin(GL_TRIANGLE_FAN);
                for (auto &q : po.points) glVertex3f(q[0], q[1], q[2]);
                glEnd();
            }
            // (the outline sits on the walls around the opening: drawn a little towards the camera, as lshade::draw
            // lets it be 0.2% + 0.03 behind them)
            glDepthFunc(GL_LEQUAL);
            glLineWidth(2.0f);
            glColor4f(1.0f, 0.85f, 0.2f, 0.9f);
            for (auto &po : p->portals) {
                glBegin(GL_LINE_LOOP);
                for (auto &q : po.points) {
                    double v[3] = {q[0] - c.eye[0], q[1] - c.eye[1], q[2] - c.eye[2]};
                    double dist = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
                    double k = dist > 1e-6 ? std::max(0.0, 1.0 - (dist * 0.002 + 0.03) / dist) : 1.0;
                    glVertex3d(c.eye[0] + v[0] * k, c.eye[1] + v[1] * k, c.eye[2] + v[2] * k);
                }
                glEnd();
            }
            glLineWidth(1.0f);
            glDepthMask(GL_TRUE);
            glDisable(GL_BLEND);
        }
    }
    SwapBuffers(d->dc);
}

}   // namespace glpreview
