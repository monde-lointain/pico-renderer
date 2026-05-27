// inspector.h — minimal ImGui frame inspector for the host harness.
//
// Opens an SDL3 window, uploads an RGB565 framebuffer to a streaming texture,
// and shows it alongside a few stage toggles. This is the ONE harness component
// that links ImGui — it is the Orthodoxy carve-out (see CLAUDE.md "Pragmatism"
// and the platform_sdl.cc precedent). NOT orthodoxy_enforced.
//
// In headless CI this target need only BUILD + LINK; interactive run is manual.
#ifndef HARNESS_INSPECTOR_H
#define HARNESS_INSPECTOR_H

#include <stdint.h>

// Per-stage display toggles the inspector exposes (extended as modules land).
struct InspectorToggles {
  int show_transform;  // draw transformed-vertex overlay (stub seam)
  int show_raster;     // draw the rasterized framebuffer
  int show_grid;       // tile-grid overlay
};

struct Inspector;  // opaque; defined in inspector.cc

// Lifecycle. inspector_create() returns NULL on failure (e.g. no display).
struct Inspector* inspector_create(const char* title, int width, int height);
void inspector_destroy(struct Inspector* insp);

// Pump one frame: process events, present `fb565` (width*height RGB565 pixels)
// with the toggle panel. Returns 0 to keep running, nonzero on quit request.
int inspector_frame(struct Inspector* insp, const uint16_t* fb565,
                    struct InspectorToggles* toggles);

#endif  // HARNESS_INSPECTOR_H
