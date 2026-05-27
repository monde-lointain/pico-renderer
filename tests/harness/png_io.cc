// png_io.cc — minimal PNG read/write. See png_io.h for scope.
//
// Writer: 8-bit RGB, filter 0 per row, single zlib stream of STORED (type 0)
// DEFLATE blocks. Reader: parses IHDR/IDAT/IEND, inflates STORED blocks,
// undoes the per-row filter (filters 0..4 supported), returns RGB8.
//
// CRC32 (PNG chunk) and Adler32 (zlib) are computed directly per spec.

#include "png_io.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

// ---- CRC32 (PNG / zlib polynomial 0xEDB88320) ------------------------------
static uint32_t crc32_update(uint32_t crc, const uint8_t* buf, size_t len) {
  crc = crc ^ 0xFFFFFFFFU;
  for (size_t i = 0; i < len; ++i) {
    crc ^= buf[i];
    for (int k = 0; k < 8; ++k) {
      uint32_t const mask = (uint32_t)(-(int32_t)(crc & 1U));
      crc = (crc >> 1) ^ (0xEDB88320U & mask);
    }
  }
  return crc ^ 0xFFFFFFFFU;
}

// ---- Adler32 (zlib check value) --------------------------------------------
static uint32_t adler32(const uint8_t* data, size_t len) {
  uint32_t a = 1;
  uint32_t b = 0;
  for (size_t i = 0; i < len; ++i) {
    a = (a + data[i]) % 65521U;
    b = (b + a) % 65521U;
  }
  return (b << 16) | a;
}

// ---- big-endian write helpers ----------------------------------------------
static void put_be32(uint8_t* p, uint32_t v) {
  p[0] = (uint8_t)(v >> 24);
  p[1] = (uint8_t)(v >> 16);
  p[2] = (uint8_t)(v >> 8);
  p[3] = (uint8_t)v;
}
static uint32_t get_be32(const uint8_t* p) {
  return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) |
         ((uint32_t)p[2] << 8) | (uint32_t)p[3];
}

// Write one PNG chunk (type[4] + data) with length prefix and CRC.
static int write_chunk(FILE* f, const char* type, const uint8_t* data,
                       uint32_t len) {
  uint8_t hdr[8];
  put_be32(hdr, len);
  memcpy(hdr + 4, type, 4);
  if (fwrite(hdr, 1, 8, f) != 8) {
    return 1;
  }
  if (len && fwrite(data, 1, len, f) != len) {
    return 1;
  }
  // CRC is over type[4]+data in a single pass (crc32_update applies the
  // PNG inversion internally, so it must see the whole chunk at once).
  uint8_t* tmp = (uint8_t*)malloc(4 + len);
  if (!tmp) {
    return 1;
  }
  memcpy(tmp, type, 4);
  if (len) {
    memcpy(tmp + 4, data, len);
  }
  uint32_t const crc_final = crc32_update(0, tmp, 4 + len);
  free(tmp);
  uint8_t crcb[4];
  put_be32(crcb, crc_final);
  return (fwrite(crcb, 1, 4, f) == 4) ? 0 : 1;
}

int png_write_rgb8(const char* path, const uint8_t* rgb, int width,
                   int height) {
  if (width <= 0 || height <= 0) {
    return 1;
  }
  FILE* f = fopen(path, "wb");
  if (!f) {
    return 1;
  }

  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (fwrite(sig, 1, 8, f) != 8) {
    fclose(f);
    return 1;
  }

  // IHDR: width, height, bitdepth=8, colortype=2 (RGB), 0,0,0
  uint8_t ihdr[13];
  put_be32(ihdr + 0, (uint32_t)width);
  put_be32(ihdr + 4, (uint32_t)height);
  ihdr[8] = 8;   // bit depth
  ihdr[9] = 2;   // color type RGB
  ihdr[10] = 0;  // compression
  ihdr[11] = 0;  // filter
  ihdr[12] = 0;  // interlace
  if (write_chunk(f, "IHDR", ihdr, 13)) {
    fclose(f);
    return 1;
  }

  // Raw image data: each row prefixed with filter byte 0.
  size_t const row_bytes = (size_t)width * 3;
  size_t const raw_len = (row_bytes + 1) * (size_t)height;
  uint8_t* raw = (uint8_t*)malloc(raw_len);
  if (!raw) {
    fclose(f);
    return 1;
  }
  for (int y = 0; y < height; ++y) {
    uint8_t* dst = raw + ((size_t)y * (row_bytes + 1));
    dst[0] = 0;  // filter: none
    memcpy(dst + 1, rgb + ((size_t)y * row_bytes), row_bytes);
  }

  // zlib stream: 2-byte header + STORED DEFLATE blocks + 4-byte Adler32.
  // STORED block: 1 byte (BFINAL/BTYPE) + 2 LEN + 2 ~LEN + LEN data.
  // Max LEN per block = 65535.
  size_t const max_block = 65535;
  size_t n_blocks = (raw_len + max_block - 1) / max_block;
  if (n_blocks == 0) {
    n_blocks = 1;
  }
  size_t const zlen = 2 + (n_blocks * 5) + raw_len + 4;
  uint8_t* z = (uint8_t*)malloc(zlen);
  if (!z) {
    free(raw);
    fclose(f);
    return 1;
  }
  size_t zi = 0;
  z[zi++] = 0x78;  // CMF: deflate, 32K window
  z[zi++] = 0x01;  // FLG: no dict, check bits make (0x78<<8|0x01)%31==0
  size_t off = 0;
  for (size_t blk = 0; blk < n_blocks; ++blk) {
    size_t this_len = raw_len - off;
    if (this_len > max_block) {
      this_len = max_block;
    }
    int const final = (blk + 1 == n_blocks) ? 1 : 0;
    z[zi++] = (uint8_t) final;  // BFINAL in bit0, BTYPE=00 (stored)
    z[zi++] = (uint8_t)(this_len & 0xFF);
    z[zi++] = (uint8_t)((this_len >> 8) & 0xFF);
    uint16_t const nlen = (uint16_t)(~this_len);
    z[zi++] = (uint8_t)(nlen & 0xFF);
    z[zi++] = (uint8_t)((nlen >> 8) & 0xFF);
    memcpy(z + zi, raw + off, this_len);
    zi += this_len;
    off += this_len;
  }
  uint32_t const ad = adler32(raw, raw_len);
  put_be32(z + zi, ad);
  zi += 4;

  int const rc = write_chunk(f, "IDAT", z, (uint32_t)zi);
  free(z);
  free(raw);
  if (rc) {
    fclose(f);
    return 1;
  }

  if (write_chunk(f, "IEND", NULL, 0)) {
    fclose(f);
    return 1;
  }
  fclose(f);
  return 0;
}

// ---- Reader ----------------------------------------------------------------
// Inflate supporting STORED blocks (type 0) only — sufficient for files we
// write. Returns 0 and fills *out (caller frees) on success.
static int inflate_stored(const uint8_t* z, size_t zlen, uint8_t** out,
                          size_t* out_len) {
  if (zlen < 6) {
    return 1;
  }
  // Skip 2-byte zlib header; trailing 4 bytes are Adler32.
  size_t i = 2;
  size_t const end = zlen - 4;
  // Worst-case output: bounded by remaining input.
  uint8_t* buf = (uint8_t*)malloc(zlen);
  if (!buf) {
    return 1;
  }
  size_t cap = zlen;
  size_t n = 0;
  int final = 0;
  while (!final) {
    if (i >= end) {
      free(buf);
      return 1;
    }
    uint8_t const hdr = z[i++];
    final = hdr & 1;
    int const btype = (hdr >> 1) & 3;
    if (btype != 0) {
      free(buf);
      return 1;
    }  // only STORED supported
    if (i + 4 > end) {
      free(buf);
      return 1;
    }
    uint16_t const len = (uint16_t)(z[i] | (z[i + 1] << 8));
    i += 4;  // skip LEN + NLEN
    if (i + len > zlen) {
      free(buf);
      return 1;
    }
    if (n + len > cap) {
      cap = n + len + 64;
      uint8_t* nb = (uint8_t*)realloc(buf, cap);
      if (!nb) {
        free(buf);
        return 1;
      }
      buf = nb;
    }
    memcpy(buf + n, z + i, len);
    n += len;
    i += len;
  }
  *out = buf;
  *out_len = n;
  return 0;
}

static uint8_t paeth(uint8_t a, uint8_t b, uint8_t c) {
  int const p = (int)a + (int)b - (int)c;
  int const pa = p > a ? p - a : a - p;
  int const pb = p > b ? p - b : b - p;
  int const pc = p > c ? p - c : c - p;
  if (pa <= pb && pa <= pc) {
    return a;
  }
  if (pb <= pc) {
    return b;
  }
  return c;
}

int png_read_rgb8(const char* path, uint8_t** out_rgb, int* out_w, int* out_h) {
  FILE* f = fopen(path, "rb");
  if (!f) {
    return 1;
  }
  fseek(f, 0, SEEK_END);
  long const sz = ftell(f);
  fseek(f, 0, SEEK_SET);
  if (sz < 8) {
    fclose(f);
    return 1;
  }
  uint8_t* file = (uint8_t*)malloc((size_t)sz);
  if (!file) {
    fclose(f);
    return 1;
  }
  if (fread(file, 1, (size_t)sz, f) != (size_t)sz) {
    free(file);
    fclose(f);
    return 1;
  }
  fclose(f);

  static const uint8_t sig[8] = {137, 80, 78, 71, 13, 10, 26, 10};
  if (memcmp(file, sig, 8) != 0) {
    free(file);
    return 1;
  }

  int width = 0;
  int height = 0;
  uint8_t* idat = NULL;
  size_t idat_len = 0;
  size_t idat_cap = 0;

  size_t p = 8;
  while (p + 8 <= (size_t)sz) {
    uint32_t const len = get_be32(file + p);
    const char* type = (const char*)(file + p + 4);
    const uint8_t* data = file + p + 8;
    if (p + 12 + len > (size_t)sz) {
      break;
    }
    if (memcmp(type, "IHDR", 4) == 0) {
      width = (int)get_be32(data);
      height = (int)get_be32(data + 4);
      if (data[8] != 8 || data[9] != 2 || data[12] != 0) {
        free(file);
        free(idat);
        return 1;  // require 8-bit RGB, no interlace
      }
    } else if (memcmp(type, "IDAT", 4) == 0) {
      uint8_t* ni = (uint8_t*)realloc(idat, idat_cap + len);
      if (!ni) {
        free(file);
        free(idat);
        return 1;
      }
      idat = ni;
      idat_cap += len;
      memcpy(idat + idat_len, data, len);
      idat_len += len;
    } else if (memcmp(type, "IEND", 4) == 0) {
      break;
    }
    p += 12 + len;  // length + type + data + crc
  }
  free(file);
  if (width <= 0 || height <= 0 || !idat) {
    free(idat);
    return 1;
  }

  uint8_t* raw = NULL;
  size_t raw_len = 0;
  if (inflate_stored(idat, idat_len, &raw, &raw_len)) {
    free(idat);
    free(raw);
    return 1;
  }
  free(idat);

  size_t const row_bytes = (size_t)width * 3;
  if (raw_len < (row_bytes + 1) * (size_t)height) {
    free(raw);
    return 1;
  }

  uint8_t* rgb = (uint8_t*)malloc(row_bytes * (size_t)height);
  if (!rgb) {
    free(raw);
    return 1;
  }

  // Undo per-row filters (0 none, 1 sub, 2 up, 3 avg, 4 paeth). bpp=3.
  for (int y = 0; y < height; ++y) {
    uint8_t const ftype = raw[(size_t)y * (row_bytes + 1)];
    const uint8_t* src = raw + ((size_t)y * (row_bytes + 1)) + 1;
    uint8_t* dst = rgb + ((size_t)y * row_bytes);
    const uint8_t* prev = (y > 0) ? rgb + ((size_t)(y - 1) * row_bytes) : NULL;
    for (size_t x = 0; x < row_bytes; ++x) {
      uint8_t const a = (x >= 3) ? dst[x - 3] : 0;
      uint8_t const b = prev ? prev[x] : 0;
      uint8_t const c = (prev && x >= 3) ? prev[x - 3] : 0;
      uint8_t const v = src[x];
      switch (ftype) {
        case 0:
          dst[x] = v;
          break;
        case 1:
          dst[x] = (uint8_t)(v + a);
          break;
        case 2:
          dst[x] = (uint8_t)(v + b);
          break;
        case 3:
          dst[x] = (uint8_t)(v + ((a + b) >> 1));
          break;
        case 4:
          dst[x] = (uint8_t)(v + paeth(a, b, c));
          break;
        default:
          free(raw);
          free(rgb);
          return 1;
      }
    }
  }
  free(raw);
  *out_rgb = rgb;
  *out_w = width;
  *out_h = height;
  return 0;
}
