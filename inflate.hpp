// Halo Map Porter - a small zlib/deflate decoder and encoder (RFC 1950/1951), for compressed Xbox cache files.
#pragma once
#include <cstddef>
#include <cstdint>
#include <string>

namespace inflate {
// Decode a zlib stream (2-byte header, deflate data). `expect` = the decoded size if known (0 = unknown).
// Returns false on corrupt data; `out` then holds what was decoded so far.
bool zlib(const uint8_t *in, size_t n, std::string &out, size_t expect = 0);

// Encode as a zlib stream: LZ77 (32 KB window, hash chains) with deflate's fixed Huffman codes. Not as small as zlib's
// best (a few percent more), but any inflater reads it.
std::string zlib_compress(const uint8_t *in, size_t n);
}
