#ifndef APP_APP_H
#define APP_APP_H

#include <stdint.h>

#include "game/rng.h"
#include "gfx/framebuffer.h"
#include "platform/platform.h"

/* App state machine states. */
enum AppState { APP_TITLE = 0, APP_PLAYING = 1 };

/* One-shot audio cues.  app_tick sets pending_cue; main_* reads+clears it via
 * app_take_cue().  CUE_NONE (0) means no cue this frame. */
enum Cue {
  CUE_NONE = 0,
  CUE_SELECT = 1, /* A pressed / state advanced */
  CUE_MOVE = 2    /* movable object moved this frame */
};

/* Full application state.
 * Calling code (main_sdl / main_pico) owns the loop; it calls:
 *   app_init(&app, seed)            once at startup
 *   app_tick(&app, in, now_ms, fb)  each frame (30fps)
 *
 * The App does NOT call plat_*; it is fully testable without a platform.
 *
 * This is the template's placeholder game: a TITLE screen, then a movable
 * object you drive with the D-pad.  Replace it with your own game. */
typedef struct {
  enum AppState state;
  int16_t obj_x;        /* movable object top-left x */
  int16_t obj_y;        /* movable object top-left y */
  Rng rng;              /* seeded PRNG (object start position) */
  uint32_t seed;        /* stored seed */
  enum Cue pending_cue; /* set by app_tick, cleared by app_take_cue */
} App;

/* Initialise app; seed feeds the demo PRNG. */
void app_init(App* a, uint32_t seed);

/* Advance one 30fps frame: consume input, update state, render into fb.
 * now_ms is the platform clock (unused by this demo). */
void app_tick(App* a, const struct Input* in, uint32_t now_ms,
              struct Framebuffer* fb);

/* Accessors for tests. */
enum AppState app_state(const App* a);
int app_obj_x(const App* a);
int app_obj_y(const App* a);

/* Consume and return the pending audio cue (clears it to CUE_NONE).
 * Call once per frame after app_tick; returns CUE_NONE if no cue. */
enum Cue app_take_cue(App* a);

#endif /* APP_APP_H */
