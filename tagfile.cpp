// Halo Map Porter - reading Halo CE tag files directly (see tagfile.hpp)
#include "tagfile.hpp"
#include <algorithm>
#include <cstring>
#include <fstream>
#include <iterator>
#include <map>

namespace tagfile {
#include "tag_layouts.inc"

static const size_t kHeader = 64;
static const size_t kStructs = sizeof(kTagStructs) / sizeof(kTagStructs[0]);

// Where one field's tag reference path, block elements or data are (kind 1/2/3), or a struct inside (4).
struct Extra {
    int field = -1, item = 0;
    size_t off = 0, len = 0;   // path: offset, length; block: first element, count; data: offset, size
    std::vector<Node> elems;   // a block's elements (when their struct has references, blocks or data)
    std::vector<Node> inner;   // a struct inside the struct (one)
};
struct Node { std::vector<Extra> extras; };

static uint32_t be32(const std::string &b, size_t o) {
    if (o + 4 > b.size()) return 0;
    const unsigned char *p = (const unsigned char *)b.data() + o;
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | p[3];
}
static uint16_t be16(const std::string &b, size_t o) {
    if (o + 2 > b.size()) return 0;
    return (uint16_t)((unsigned char)b[o] << 8 | (unsigned char)b[o + 1]);
}

static size_t field_bytes(const TagLayoutField &f) {
    switch (f.kind) {
    case 1: return 16;
    case 2: return 12;
    case 3: return 20;
    case 4: return kTagStructs[f.sub].size;
    default:
        switch (f.type) {
        case 'b': case 'B': return 1;
        case 'h': case 'H': return 2;
        case 's': return 32;
        default: return 4;
        }
    }
}

// Whether a struct (or anything inside it) has data after it in the file.
static bool has_extras(int sid) {
    static std::vector<signed char> memo(kStructs, -1);
    if (memo[sid] >= 0) return memo[sid] != 0;
    memo[sid] = 0;   // (a struct can't contain itself)
    const TagLayoutStruct &s = kTagStructs[sid];
    bool any = false;
    for (unsigned i = s.first; i < (unsigned)s.first + s.fields && !any; i++) {
        const TagLayoutField &f = kTagFields[i];
        any = f.kind == 1 || f.kind == 2 || f.kind == 3 || (f.kind == 4 && has_extras(f.sub));
    }
    memo[sid] = any ? 1 : 0;
    return any;
}

// Reads the references, blocks and data of the struct at `off`, which start at `pos` (moved past them).
static bool walk(const std::string &b, int sid, size_t off, Node &node, size_t &pos, int depth) {
    if (depth > 32) return false;
    const TagLayoutStruct &s = kTagStructs[sid];
    for (unsigned i = s.first; i < (unsigned)s.first + s.fields; i++) {
        const TagLayoutField &f = kTagFields[i];
        if (f.kind == 0) continue;
        if (f.kind == 4 && !has_extras(f.sub)) continue;
        size_t fb = field_bytes(f);
        for (int item = 0; item < f.count; item++) {
            size_t at = off + f.offset + item * fb;
            if (at + fb > b.size()) return false;
            Extra e;
            e.field = (int)i;
            e.item = item;
            if (f.kind == 1) {
                uint32_t len = be32(b, at + 8);
                if (len) {
                    if (len > 1024 || pos + len + 1 > b.size()) return false;
                    e.off = pos;
                    e.len = len;
                    pos += len + 1;
                }
            } else if (f.kind == 2) {
                uint32_t count = be32(b, at);
                size_t size = kTagStructs[f.sub].size;
                if (count > 1000000 || pos + (size_t)count * size > b.size()) return false;
                e.off = pos;
                e.len = count;
                pos += (size_t)count * size;
                if (has_extras(f.sub)) {
                    e.elems.resize(count);
                    for (uint32_t k = 0; k < count; k++)
                        if (!walk(b, f.sub, e.off + k * size, e.elems[k], pos, depth + 1)) return false;
                }
            } else if (f.kind == 3) {
                uint32_t size = be32(b, at);
                if (pos + size > b.size()) return false;
                e.off = pos;
                e.len = size;
                pos += size;
            } else {
                e.inner.resize(1);
                if (!walk(b, f.sub, at, e.inner[0], pos, depth + 1)) return false;
            }
            node.extras.push_back(std::move(e));
        }
    }
    return true;
}

int root_struct_for(const std::string &extension) {
    for (auto &r : kTagRoots)
        if (extension == r.extension)
            for (size_t i = 0; i < kStructs; i++)
                if (!strcmp(kTagStructs[i].name, r.root)) return (int)i;
    return -1;
}

Tag::Tag() = default;
Tag::~Tag() = default;

bool Tag::load(const std::filesystem::path &file) {
    root_.reset();
    root_sid_ = root_struct_for(file.extension().u8string().substr(file.extension().empty() ? 0 : 1));
    if (root_sid_ < 0) return false;
    std::ifstream in(file, std::ios::binary);
    b_.assign((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (b_.size() < kHeader + kTagStructs[root_sid_].size || b_.compare(60, 4, "blam") != 0) return false;
    auto node = std::make_unique<Node>();
    size_t pos = kHeader + kTagStructs[root_sid_].size;
    if (!walk(b_, root_sid_, kHeader, *node, pos, 0) || pos != b_.size()) return false;
    root_ = std::move(node);
    return true;
}

Elem Tag::root() const {
    Elem e;
    if (!root_) return e;
    e.tag_ = this;
    e.sid_ = root_sid_;
    e.off_ = kHeader;
    e.node_ = root_.get();
    return e;
}

int Elem::field(const char *name, int kind) const {
    if (!tag_) return -1;
    const TagLayoutStruct &s = kTagStructs[sid_];
    for (unsigned i = s.first; i < (unsigned)s.first + s.fields; i++)
        if (kTagFields[i].kind == kind && !strcmp(kTagFields[i].name, name)) return (int)i;
    return -1;
}

static const Extra *find_extra(const Node *node, int field) {
    if (!node) return nullptr;
    for (auto &e : node->extras)
        if (e.field == field && e.item == 0) return &e;
    return nullptr;
}

double Elem::num(const char *name, int k) const {
    int i = field(name, 0);
    if (i < 0 || k < 0 || k >= kTagFields[i].count) return 0;
    const TagLayoutField &f = kTagFields[i];
    const std::string &b = tag_->b_;
    size_t at = off_ + f.offset + k * field_bytes(f);
    switch (f.type) {
    case 'f': { uint32_t u = be32(b, at); float v; memcpy(&v, &u, 4); return v; }
    case 'b': return at < b.size() ? (double)(int8_t)b[at] : 0;
    case 'B': return at < b.size() ? (double)(uint8_t)b[at] : 0;
    case 'h': return (int16_t)be16(b, at);
    case 'H': return be16(b, at);
    case 'i': return (int32_t)be32(b, at);
    case 'I': return be32(b, at);
    default: return 0;
    }
}

std::string Elem::str(const char *name) const {
    int i = field(name, 0);
    if (i < 0 || kTagFields[i].type != 's') return "";
    size_t at = off_ + kTagFields[i].offset;
    if (at + 32 > tag_->b_.size()) return "";
    std::string s = tag_->b_.substr(at, 32);
    return s.substr(0, s.find('\0'));
}

static const char *extension_of(const std::string &group) {
    static const std::map<std::string, const char *> ext{
        {"mode", "model"}, {"mod2", "gbxmodel"}, {"sbsp", "scenario_structure_bsp"}, {"scnr", "scenario"},
        {"bipd", "biped"}, {"vehi", "vehicle"}, {"weap", "weapon"}, {"eqip", "equipment"}, {"garb", "garbage"},
        {"proj", "projectile"}, {"scen", "scenery"}, {"mach", "device_machine"}, {"ctrl", "device_control"},
        {"lifi", "device_light_fixture"}, {"plac", "placeholder"}, {"ssce", "sound_scenery"}, {"itmc", "item_collection"},
        {"antr", "model_animations"}, {"coll", "model_collision_geometry"}, {"bitm", "bitmap"}, {"snd!", "sound"}};
    auto it = ext.find(group);
    return it == ext.end() ? nullptr : it->second;
}

std::string Elem::ref(const char *name, std::string *group) const {
    int i = field(name, 1);
    if (i < 0) return "";
    const std::string &b = tag_->b_;
    size_t at = off_ + kTagFields[i].offset;
    std::string g = at + 4 <= b.size() ? b.substr(at, 4) : "";
    if (group) *group = g;
    const Extra *e = find_extra(node_, i);
    if (!e || !e->len) return "";
    std::string path = b.substr(e->off, e->len);
    std::replace(path.begin(), path.end(), '\\', '/');
    const char *x = extension_of(g);
    return x ? path + "." + x : path;
}

Block Elem::block(const char *name) const {
    Block r;
    int i = field(name, 2);
    const Extra *e = i < 0 ? nullptr : find_extra(node_, i);
    if (!e) return r;
    r.tag_ = tag_;
    r.sid_ = kTagFields[i].sub;
    r.base_ = e->off;
    r.count_ = e->len;
    r.nodes_ = e->elems.empty() ? nullptr : &e->elems;
    return r;
}

std::string Elem::data(const char *name) const {
    int i = field(name, 3);
    const Extra *e = i < 0 ? nullptr : find_extra(node_, i);
    return e ? tag_->b_.substr(e->off, e->len) : std::string();
}

Elem Elem::inner(const char *name) const {
    Elem r;
    int i = field(name, 4);
    if (i < 0) return r;
    r.tag_ = tag_;
    r.sid_ = kTagFields[i].sub;
    r.off_ = off_ + kTagFields[i].offset;
    const Extra *e = find_extra(node_, i);
    r.node_ = e && !e->inner.empty() ? &e->inner[0] : nullptr;
    return r;
}

Elem Block::operator[](size_t i) const {
    Elem e;
    if (!tag_ || i >= count_) return e;
    e.tag_ = tag_;
    e.sid_ = sid_;
    e.off_ = base_ + i * kTagStructs[sid_].size;
    e.node_ = nodes_ ? &(*nodes_)[i] : nullptr;
    return e;
}

}   // namespace tagfile
