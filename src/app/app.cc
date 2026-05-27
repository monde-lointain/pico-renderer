/* app.cc — template placeholder game: TITLE screen + movable object.
 *
 * Demonstrates the engine contract end to end: input (D-pad + A), rendering
 * (gfx primitives + a sprite blit), the cue/audio bridge, and the PRNG.
 * Replace this with your own game. */
#include "app/app.h"

#include "gfx/blit.h"
#include "gfx/font.h"

/* Plain ints (not an enum) so arithmetic with the framebuffer's
 * screen-dimension enum (SCREEN_W/SCREEN_H) doesn't mix two enum types
 * (deprecated in C++20). */
// clang-format off
static const int OBJ_SIZE = 16; /* movable object is a 16x16 filled square */
static const int OBJ_STEP = 4; /* pixels moved per frame while a D-pad button is held */
// clang-format on

static int16_t clamp_i16(int v, int lo, int hi) {
  if (v < lo) {
    return (int16_t)lo;
  }
  if (v > hi) {
    return (int16_t)hi;
  }
  return (int16_t)v;
}

void app_init(App* a, uint32_t seed) {
  rng_seed(&a->rng, seed);
  a->seed = seed;
  a->state = APP_TITLE;
  a->obj_x = (int16_t)((SCREEN_W - OBJ_SIZE) / 2);
  a->obj_y = (int16_t)((SCREEN_H - OBJ_SIZE) / 2);
  a->pending_cue = CUE_NONE;
}

enum AppState app_state(const App* a) { return a->state; }
int app_obj_x(const App* a) { return (int)a->obj_x; }
int app_obj_y(const App* a) { return (int)a->obj_y; }

enum Cue app_take_cue(App* a) {
  const enum Cue c = a->pending_cue;
  a->pending_cue = CUE_NONE;
  return c;
}

static void render_title(struct Framebuffer* fb) {
  frect(fb, 0, 0, SCREEN_W, SCREEN_H, rgb565(8, 12, 40));
  text(fb, "PICOSYSTEM", 48, 70, rgb565(255, 220, 0));
  text(fb, "TEMPLATE", 64, 100, rgb565(255, 255, 255));
  text(fb, "PRESS A", 72, 150, rgb565(120, 200, 120));
}

static void render_play(const App* a, struct Framebuffer* fb) {
  frect(fb, 0, 0, SCREEN_W, SCREEN_H, rgb565(16, 16, 24));
  /* The movable object, drawn as a filled rect. */
  frect(fb, (int)a->obj_x, (int)a->obj_y, OBJ_SIZE, OBJ_SIZE,
        rgb565(255, 80, 80));
  text(fb, "MOVE X Y", 8, SCREEN_H - 28, rgb565(160, 160, 160));
}

void app_tick(App* a, const struct Input* in, uint32_t now_ms,
              struct Framebuffer* fb) {
  (void)now_ms;

  if (a->state == APP_TITLE) {
    if (in->pressed & BTN_A) {
      a->state = APP_PLAYING;
      /* rng-derived start position, kept fully on screen. */
      a->obj_x = (int16_t)(rng_next(&a->rng) % (uint32_t)(SCREEN_W - OBJ_SIZE));
      a->obj_y = (int16_t)(rng_next(&a->rng) % (uint32_t)(SCREEN_H - OBJ_SIZE));
      a->pending_cue = CUE_SELECT;
    }
    render_title(fb);
    return;
  }

  /* APP_PLAYING: D-pad moves the object; A beeps. */
  int x = (int)a->obj_x;
  int y = (int)a->obj_y;
  int moved = 0;
  if (in->held & BTN_LEFT) {
    x -= OBJ_STEP;
    moved = 1;
  }
  if (in->held & BTN_RIGHT) {
    x += OBJ_STEP;
    moved = 1;
  }
  if (in->held & BTN_UP) {
    y -= OBJ_STEP;
    moved = 1;
  }
  if (in->held & BTN_DOWN) {
    y += OBJ_STEP;
    moved = 1;
  }
  a->obj_x = clamp_i16(x, 0, SCREEN_W - OBJ_SIZE);
  a->obj_y = clamp_i16(y, 0, SCREEN_H - OBJ_SIZE);

  if (in->pressed & BTN_A) {
    a->pending_cue = CUE_SELECT;
  } else if (moved) {
    a->pending_cue = CUE_MOVE;
  }

  render_play(a, fb);
}
