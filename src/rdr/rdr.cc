#include "rdr/rdr.h"

// Frame state owned by the renderer (POD). Wired to modules at C1.
struct Frame {
  uint16_t* fb;
  int width;
  int height;
};

RdrErr rdr_begin_frame(struct Frame* f) {
  (void)f;
  return RDR_OK;
}
RdrErr rdr_submit(struct Frame* f, const struct Command* cmds) {
  (void)f;
  (void)cmds;
  return RDR_OK;
}
RdrErr rdr_end_frame(struct Frame* f) {
  (void)f;
  return RDR_OK;
}
