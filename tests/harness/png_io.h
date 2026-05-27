// png_io.h — minimal, dependency-free PNG read/write for the golden framework.
//
// Scope: 8-bit RGB (color type 2), no interlace, single IDAT using STORED
// (uncompressed) DEFLATE blocks. This is deliberately tiny — the harness owns
// the only PNGs it touches (golden images it writes and reads back), so we do
// not need a general decoder. Orthodox C++ (this file is NOT orthodoxy_enforced
// — it lives under the ImGui/harness carve-out — but it stays C-like anyway).
//
// All buffers are RGB8 row-major, 3 bytes/pixel, width*height*3 bytes.
#ifndef HARNESS_PNG_IO_H
#define HARNESS_PNG_IO_H

#include <stddef.h>
#include <stdint.h>

// Write an 8-bit RGB image to `path`. `rgb` is width*height*3 bytes, row-major.
// Returns 0 on success, nonzero on failure (open/write error).
int png_write_rgb8(const char* path, const uint8_t* rgb, int width, int height);

// Read an 8-bit RGB PNG written by png_write_rgb8 (or any 8-bit RGB,
// non-interlaced PNG using zlib/DEFLATE, stored or compressed).
// On success: allocates *out_rgb (caller frees with free()), sets dimensions,
// returns 0. On failure returns nonzero and leaves outputs untouched.
int png_read_rgb8(const char* path, uint8_t** out_rgb, int* out_w, int* out_h);

#endif  // HARNESS_PNG_IO_H
