// demo/demo_scene.h — testable scene builder for the C1 demo. Orthodox C++.
//
// Factored OUT of main()'s for(;;) loop so the ACTUAL demo scene (the gldemo
// flat pyramid + cube, same geometry/matrices the device runs) can be driven
// through the rdr pipeline by a host guard test — the black-screen regression
// (camera behind the geometry -> every tri near-rejected) escaped precisely
// because the demo's own build path was never exercised on host. The builder
// fills a caller-provided Command buffer (END-terminated) for the given
// animation angles; matrices the stream references are held in module-local
// storage that outlives the returned stream until the next build call.
#ifndef DEMO_SCENE_H
#define DEMO_SCENE_H

#include "rdr/types.h"

// Recommended command-buffer capacity for the scene (pyramid + cube + state).
enum { DEMO_CMD_CAP = 64 };

// Build the flat scene into `buf` (capacity `cap` Commands) at the given
// pyramid/cube rotation angles (degrees). The stream is END-terminated. Returns
// the command count written (0 if cap is too small). The referenced matrices
// live in module storage valid until the next demo_scene_build call.
uint32_t demo_scene_build(struct Command* buf, uint32_t cap, float pyr_angle,
                          float cube_angle);

#endif  // DEMO_SCENE_H
