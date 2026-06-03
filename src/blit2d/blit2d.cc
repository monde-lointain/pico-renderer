// blit2d.cc — implementation (Wave-D stream D.5: blit2d). Orthodox C++.
#include "blit2d/blit2d.h"

// Contract-first stubs (return RDR_OK / zero so the library links). Behavior
// lands via the orthodox-tdd-cycle.

int blit2d_horizon_row_1to1(int src_horizon_row, int src_h, int dst_h) {
  (void)src_horizon_row;
  (void)src_h;
  (void)dst_h;
  return 0;
}

uint16_t blit2d_decode_ci8(const void* ci8, const void* tlut, int w, int x,
                           int y) {
  (void)ci8;
  (void)tlut;
  (void)w;
  (void)x;
  (void)y;
  return 0;
}

uint8_t blit2d_decode_i8(const void* i8, int w, int x, int y) {
  (void)i8;
  (void)w;
  (void)x;
  (void)y;
  return 0;
}

RdrErr blit2d_panorama(const struct Blit2dRect* r, uint16_t* fb) {
  (void)r;
  (void)fb;
  return RDR_OK;
}

RdrErr blit2d_clouds(const struct Blit2dRect* r, uint16_t* fb) {
  (void)r;
  (void)fb;
  return RDR_OK;
}

RdrErr blit2d_render(const struct Blit2dRect* r, uint16_t* fb) {
  (void)r;
  (void)fb;
  return RDR_OK;
}
