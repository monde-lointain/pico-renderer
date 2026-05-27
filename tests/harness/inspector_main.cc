// inspector_main.cc — manual driver for the ImGui frame inspector.
//
// Purpose in CI: prove the inspector target BUILDS + LINKS against SDL3/ImGui
// (the binary is built, not run, in headless CI). Run manually to view a frame.
// With no display available, create() returns NULL and we exit 0 cleanly.

#include <stdio.h>
#include <stdlib.h>

#include "gfx/framebuffer.h"
#include "inspector.h"

int main(void) {
  struct Inspector* insp =
      inspector_create("Harness Inspector", SCREEN_W, SCREEN_H);
  if (!insp) {
    // No display (headless) — link/build is what matters here.
    fprintf(stderr, "inspector: no display; build/link OK, skipping run\n");
    return 0;
  }

  static struct Framebuffer fb;
  for (int i = 0; i < SCREEN_PIXELS; ++i) {
    int x = i % SCREEN_W, y = i / SCREEN_W;
    fb.px[i] = rgb565((uint8_t)x, (uint8_t)y, (uint8_t)(x ^ y));
  }

  struct InspectorToggles t = {0, 1, 0};
  int frames = 0;
  while (inspector_frame(insp, fb.px, &t) == 0 && frames < 600) ++frames;
  inspector_destroy(insp);
  return 0;
}
