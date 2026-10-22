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

// ---------------------------------------------------------------- editing

static void put32(std::string &b, size_t o, uint32_t v) {
    if (o + 4 > b.size()) return;
    b[o] = (char)(v >> 24); b[o + 1] = (char)(v >> 16); b[o + 2] = (char)(v >> 8); b[o + 3] = (char)v;
}
static void put16(std::string &b, size_t o, uint16_t v) {
    if (o + 2 > b.size()) return;
    b[o] = (char)(v >> 8); b[o + 1] = (char)v;
}

// Copies the struct at `off` (bytes in `own`, or in its parent's for a struct inside a struct) and what follows it.
static bool copy_walk(const std::string &b, int sid, size_t off, TagNode &node, bool own, size_t &pos, int depth) {
    if (depth > 32) return false;
    const TagLayoutStruct &s = kTagStructs[sid];
    node.sid = sid;
    if (own) node.fixed = b.substr(off, s.size);
    for (unsigned i = s.first; i < (unsigned)s.first + s.fields; i++) {
        const TagLayoutField &f = kTagFields[i];
        if (f.kind == 0) continue;
        size_t fb = field_bytes(f);
        for (int item = 0; item < f.count; item++) {
            size_t at = off + f.offset + item * fb;
            if (at + fb > b.size()) return false;
            TagNode::Extra e;
            e.field = (int)i;
            e.item = item;
            if (f.kind == 1) {
                uint32_t len = be32(b, at + 8);
                if (len) {
                    if (len > 1024 || pos + len + 1 > b.size()) return false;
                    e.bytes = b.substr(pos, len);
                    pos += len + 1;
                }
            } else if (f.kind == 2) {
                uint32_t count = be32(b, at);
                size_t size = kTagStructs[f.sub].size, base = pos;
                if (count > 1000000 || pos + (size_t)count * size > b.size()) return false;
                pos += (size_t)count * size;
                e.elems.resize(count);
                for (uint32_t k = 0; k < count; k++)
                    if (!copy_walk(b, f.sub, base + k * size, e.elems[k], true, pos, depth + 1)) return false;
            } else if (f.kind == 3) {
                uint32_t size = be32(b, at);
                if (pos + size > b.size()) return false;
                e.bytes = b.substr(pos, size);
                pos += size;
            } else {
                e.inner.resize(1);
                if (!copy_walk(b, f.sub, at, e.inner[0], false, pos, depth + 1)) return false;
            }
            node.extras.push_back(std::move(e));
        }
    }
    return true;
}

// Sets the counts and lengths in a struct's bytes (`buf` at `base`) from what it holds.
static void fix_counts(TagNode &node, std::string &buf, size_t base) {
    for (auto &e : node.extras) {
        const TagLayoutField &f = kTagFields[e.field];
        size_t at = base + f.offset + e.item * field_bytes(f);
        if (f.kind == 1) put32(buf, at + 8, (uint32_t)e.bytes.size());
        else if (f.kind == 2) put32(buf, at, (uint32_t)e.elems.size());
        else if (f.kind == 3) put32(buf, at, (uint32_t)e.bytes.size());
        else if (f.kind == 4 && !e.inner.empty()) fix_counts(e.inner[0], buf, at);
    }
}

static void emit(TagNode &node, std::string &buf, size_t base, std::string &out) {
    for (auto &e : node.extras) {
        const TagLayoutField &f = kTagFields[e.field];
        if (f.kind == 1) {
            if (!e.bytes.empty()) { out += e.bytes; out += '\0'; }
        } else if (f.kind == 2) {
            for (auto &el : e.elems) {
                fix_counts(el, el.fixed, 0);
                out += el.fixed;
            }
            for (auto &el : e.elems) emit(el, el.fixed, 0, out);
        } else if (f.kind == 3) {
            out += e.bytes;
        } else if (!e.inner.empty()) {
            emit(e.inner[0], buf, base + f.offset + e.item * field_bytes(f), out);
        }
    }
}

static uint32_t crc32(const char *d, size_t n) {
    static uint32_t table[256];
    static bool init = false;
    if (!init) {
        for (uint32_t i = 0; i < 256; i++) {
            uint32_t c = i;
            for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1;
            table[i] = c;
        }
        init = true;
    }
    uint32_t c = 0xFFFFFFFFu;
    for (size_t i = 0; i < n; i++) c = table[(c ^ (unsigned char)d[i]) & 0xFF] ^ (c >> 8);
    return c ^ 0xFFFFFFFFu;
}

bool EditTag::load(const std::filesystem::path &file) {
    Tag t;
    if (!t.load(file)) return false;
    const std::string &b = t.bytes();
    header_ = b.substr(0, kHeader);
    root_ = TagNode();
    size_t pos = kHeader + kTagStructs[t.root_sid_].size;
    return copy_walk(b, t.root_sid_, kHeader, root_, true, pos, 0) && pos == b.size();
}

std::string EditTag::bytes() const {
    TagNode r = root_;   // (counts are written into the copy)
    fix_counts(r, r.fixed, 0);
    std::string out = header_ + r.fixed;
    emit(r, r.fixed, 0, out);
    out.replace(kHeader, r.fixed.size(), r.fixed);   // emit may have fixed counts of inner structs in r.fixed
    put32(out, 40, ~crc32(out.data() + kHeader, out.size() - kHeader));
    return out;
}

bool EditTag::save(const std::filesystem::path &file) const {
    std::string out = bytes();
    std::ofstream f(file, std::ios::binary | std::ios::trunc);
    f << out;
    return (bool)f;
}

static int field_of(int sid, const char *name, int kind) {
    if (sid < 0) return -1;
    const TagLayoutStruct &s = kTagStructs[sid];
    for (unsigned i = s.first; i < (unsigned)s.first + s.fields; i++)
        if (kTagFields[i].kind == kind && !strcmp(kTagFields[i].name, name)) return (int)i;
    return -1;
}

double get_num(const TagNode &n, const char *name, int k) {
    int i = field_of(n.sid, name, 0);
    if (i < 0 || k < 0 || k >= kTagFields[i].count) return 0;
    const TagLayoutField &f = kTagFields[i];
    size_t at = f.offset + k * field_bytes(f);
    const std::string &b = n.fixed;
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

bool set_num(TagNode &n, const char *name, double v, int k) {
    int i = field_of(n.sid, name, 0);
    if (i < 0 || k < 0 || k >= kTagFields[i].count) return false;
    const TagLayoutField &f = kTagFields[i];
    size_t at = f.offset + k * field_bytes(f);
    std::string &b = n.fixed;
    if (at + field_bytes(f) > b.size()) return false;
    switch (f.type) {
    case 'f': { float x = (float)v; uint32_t u; memcpy(&u, &x, 4); put32(b, at, u); return true; }
    case 'b': case 'B': b[at] = (char)(int)v; return true;
    case 'h': case 'H': put16(b, at, (uint16_t)(int)v); return true;
    case 'i': case 'I': put32(b, at, (uint32_t)(long long)v); return true;
    default: return false;
    }
}

std::vector<TagNode> *block(TagNode &n, const char *name) {
    int i = field_of(n.sid, name, 2);
    for (auto &e : n.extras)
        if (e.field == i && e.item == 0) return &e.elems;
    return nullptr;
}

int block_struct(const TagNode &n, const char *name) {
    int i = field_of(n.sid, name, 2);
    return i < 0 ? -1 : kTagFields[i].sub;
}

int struct_size(int sid) { return sid < 0 ? 0 : (int)kTagStructs[sid].size; }

static void init_extras(TagNode &n, int sid) {
    const TagLayoutStruct &s = kTagStructs[sid];
    for (unsigned i = s.first; i < (unsigned)s.first + s.fields; i++) {
        const TagLayoutField &f = kTagFields[i];
        if (f.kind == 0) continue;
        for (int item = 0; item < f.count; item++) {
            TagNode::Extra e;
            e.field = (int)i;
            e.item = item;
            if (f.kind == 4) {
                e.inner.resize(1);
                e.inner[0].sid = f.sub;
                init_extras(e.inner[0], f.sub);
            }
            n.extras.push_back(std::move(e));
        }
    }
}

TagNode new_node(int sid) {
    TagNode n;
    n.sid = sid;
    n.fixed.assign(kTagStructs[sid].size, '\0');
    init_extras(n, sid);
    return n;
}

static void put_u32(std::string &out, uint32_t v) {
    char b[4] = {(char)(v >> 24), (char)(v >> 16), (char)(v >> 8), (char)v};
    out.append(b, 4);
}
static bool get_u32(const std::string &b, size_t &pos, uint32_t &v) {
    if (pos + 4 > b.size()) return false;
    v = be32(b, pos);
    pos += 4;
    return true;
}
static bool get_str(const std::string &b, size_t &pos, std::string &s) {
    uint32_t n;
    if (!get_u32(b, pos, n) || pos + n > b.size()) return false;
    s = b.substr(pos, n);
    pos += n;
    return true;
}

// name, struct size, its bytes, then each extra: its field's name, item, bytes, elements, inner struct.
std::string write_node(const TagNode &n) {
    std::string out;
    const TagLayoutStruct &s = kTagStructs[n.sid];
    put_u32(out, (uint32_t)strlen(s.name));
    out += s.name;
    put_u32(out, s.size);
    put_u32(out, (uint32_t)n.fixed.size());
    out += n.fixed;
    put_u32(out, (uint32_t)n.extras.size());
    for (auto &e : n.extras) {
        const char *fname = kTagFields[e.field].name;
        put_u32(out, (uint32_t)strlen(fname));
        out += fname;
        put_u32(out, (uint32_t)e.item);
        put_u32(out, (uint32_t)e.bytes.size());
        out += e.bytes;
        put_u32(out, (uint32_t)e.elems.size());
        for (auto &el : e.elems) out += write_node(el);
        put_u32(out, (uint32_t)e.inner.size());
        for (auto &in : e.inner) out += write_node(in);
    }
    return out;
}

bool read_node(const std::string &b, size_t &pos, TagNode &n) {
    std::string name, fixed;
    uint32_t size, count;
    if (!get_str(b, pos, name) || !get_u32(b, pos, size) || !get_str(b, pos, fixed) || !get_u32(b, pos, count)) return false;
    n = TagNode();
    for (size_t i = 0; i < kStructs; i++)
        if (name == kTagStructs[i].name) n.sid = (int)i;
    if (n.sid < 0 || kTagStructs[n.sid].size != size || (!fixed.empty() && fixed.size() != size)) return false;
    n.fixed = fixed;
    const TagLayoutStruct &s = kTagStructs[n.sid];
    for (uint32_t k = 0; k < count; k++) {
        TagNode::Extra e;
        std::string fname;
        uint32_t item, ne, ni;
        if (!get_str(b, pos, fname) || !get_u32(b, pos, item) || !get_str(b, pos, e.bytes)) return false;
        for (unsigned i = s.first; i < (unsigned)s.first + s.fields; i++)
            if (kTagFields[i].kind != 0 && fname == kTagFields[i].name) e.field = (int)i;
        if (e.field < 0) return false;
        e.item = (int)item;
        if (!get_u32(b, pos, ne) || ne > 1000000) return false;
        e.elems.resize(ne);
        for (auto &el : e.elems)
            if (!read_node(b, pos, el)) return false;
        if (!get_u32(b, pos, ni) || ni > 1) return false;
        e.inner.resize(ni);
        for (auto &in : e.inner)
            if (!read_node(b, pos, in)) return false;
        n.extras.push_back(std::move(e));
    }
    return true;
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
