// Halo Map Porter - reading Halo CE tag files (HEK, big endian) directly, for the kinds in tag_layouts.inc
// (structure BSPs, models, animations, scenarios, objects, bitmaps, shaders). Much faster than invader-edit for a tag
// with thousands of elements (a BSP's 50,000 surfaces), and it reads values invader-edit doesn't print (a BSP's
// visibility data).
#pragma once
#include <cstdint>
#include <filesystem>
#include <memory>
#include <string>
#include <vector>

namespace tagfile {

struct Node;   // where an element's tag references, blocks and data are in the file

class Tag;
class Block;

// One struct in the tag: the root, or an element of a block.
class Elem {
public:
    Elem() = default;
    bool ok() const { return tag_ != nullptr; }
    // Values by field name (as in tag_layouts.inc, e.g. "bounding radius"); k = which value of a multi-value field.
    double num(const char *field, int k = 0) const;   // 0 if missing
    long long integer(const char *field, int k = 0) const { return (long long)num(field, k); }
    std::string str(const char *field) const;           // a 32-character string
    // A tag reference: "path/to/tag.extension" ('/' separators; empty if none). group: the tag's four letters.
    std::string ref(const char *field, std::string *group = nullptr) const;
    Block block(const char *field) const;
    std::string data(const char *field) const;          // a data field's bytes
    Elem inner(const char *field) const;                // a struct inside the struct

private:
    friend class Tag;
    friend class Block;
    const Tag *tag_ = nullptr;
    int sid_ = -1;
    size_t off_ = 0;
    const Node *node_ = nullptr;
    int field(const char *name, int kind) const;
};

class Block {
public:
    size_t size() const { return count_; }
    Elem operator[](size_t i) const;

private:
    friend class Elem;
    const Tag *tag_ = nullptr;
    int sid_ = -1;
    size_t base_ = 0, count_ = 0;
    const std::vector<Node> *nodes_ = nullptr;
};

class Tag {
public:
    Tag();
    ~Tag();
    // Reads a tag whose kind tag_layouts.inc has (by its extension). False if it can't be read or isn't laid out
    // as expected (it must end exactly where its last block or data does).
    bool load(const std::filesystem::path &file);
    Elem root() const;
    const std::string &bytes() const { return b_; }

private:
    friend class Elem;
    friend class Block;
    friend class EditTag;
    std::string b_;
    std::unique_ptr<Node> root_;
    int root_sid_ = -1;
};

// The struct layouts' name lookup (for tests): the root struct for a tag extension ("model"), or -1.
int root_struct_for(const std::string &extension);

// ---- Editing a tag: an editable copy, saved back with the counts, lengths and header checksum it then needs.

// A struct in an editable tag: its bytes, and its tag reference paths, block elements, data and inner structs in
// the order they're saved. (A struct inside a struct keeps its bytes in its parent's `fixed`.)
struct TagNode {
    int sid = -1;
    std::string fixed;
    struct Extra {
        int field = -1, item = 0;
        std::string bytes;            // a tag reference's path, or a data field's bytes
        std::vector<TagNode> elems;   // a block's elements
        std::vector<TagNode> inner;   // a struct inside the struct (one)
    };
    std::vector<Extra> extras;
};

class EditTag {
public:
    bool load(const std::filesystem::path &file);   // false if Tag::load would be false
    bool save(const std::filesystem::path &file) const;
    std::string bytes() const;                       // what save() writes
    TagNode &root() { return root_; }
    const TagNode &root() const { return root_; }

private:
    std::string header_;
    TagNode root_;
};

// Values of an editable struct (its own bytes; not for a struct inside a struct), by field name.
double get_num(const TagNode &n, const char *field, int k = 0);
std::string get_str(const TagNode &n, const char *field);   // a 32-character string
bool set_num(TagNode &n, const char *field, double v, int k = 0);
// A block's elements (nullptr if the struct has no such block). Elements added must be of the block's struct:
// copies of its elements, or new_node(block_struct(...)).
std::vector<TagNode> *block(TagNode &n, const char *field);
int block_struct(const TagNode &n, const char *field);
// A data field's bytes (nullptr if the struct has no such field). Saving writes its size.
std::string *data(TagNode &n, const char *field);
// A tag reference's path, '\\' separators, no extension ("" for none; nullptr if the struct has no such field).
// Saving writes its length.
std::string *ref_path(TagNode &n, const char *field);
int struct_size(int sid);
// A zero-filled struct with empty references, blocks and data.
TagNode new_node(int sid);
// A struct as bytes and back (for keeping one outside its tag, e.g. the LOD library). The struct is stored by name,
// so read_node fails if this version of the app lays it out differently.
std::string write_node(const TagNode &n);
bool read_node(const std::string &b, size_t &pos, TagNode &n);

}   // namespace tagfile
