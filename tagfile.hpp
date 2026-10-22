// Halo Map Porter - reading Halo CE tag files (HEK, big endian) directly, for the kinds in tag_layouts.inc
// (structure BSPs, models, scenarios, objects). Much faster than invader-edit for a tag with thousands of
// elements (a BSP's 50,000 surfaces), and it reads values invader-edit doesn't print (a BSP's visibility data).
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
    std::string b_;
    std::unique_ptr<Node> root_;
    int root_sid_ = -1;
};

// The struct layouts' name lookup (for tests): the root struct for a tag extension ("model"), or -1.
int root_struct_for(const std::string &extension);

}   // namespace tagfile
