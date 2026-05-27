/* app_test.cc — the template demo app's state machine, exercised without a
 * platform (App is platform-free: construct it and inject Input directly). */
#include "app/app.h"

#include <gtest/gtest.h>
#include <string.h>

#include "gfx/framebuffer.h"

static struct Input mk_input(uint32_t held, uint32_t pressed) {
  struct Input in;
  in.held = held;
  in.pressed = pressed;
  return in;
}

TEST(App, StartsInTitle) {
  App a;
  app_init(&a, 1U);
  EXPECT_EQ(app_state(&a), APP_TITLE);
}

TEST(App, PressAAdvancesToPlayingAndEmitsSelectCue) {
  App a;
  app_init(&a, 1U);
  Framebuffer fb;
  struct Input const in = mk_input(0U, BTN_A);
  app_tick(&a, &in, 0U, &fb);
  EXPECT_EQ(app_state(&a), APP_PLAYING);
  EXPECT_EQ(app_take_cue(&a), CUE_SELECT);
}

TEST(App, DpadMovesObjectInPlayingAndEmitsMoveCue) {
  App a;
  app_init(&a, 1U);
  Framebuffer fb;
  struct Input const start = mk_input(0U, BTN_A);
  app_tick(&a, &start, 0U, &fb);
  (void)app_take_cue(&a);

  int const x0 = app_obj_x(&a);
  struct Input const right = mk_input(BTN_RIGHT, 0U);
  app_tick(&a, &right, 33U, &fb);
  EXPECT_GT(app_obj_x(&a), x0);
  EXPECT_EQ(app_take_cue(&a), CUE_MOVE);
}

TEST(App, ObjectStaysOnScreen) {
  App a;
  app_init(&a, 1U);
  Framebuffer fb;
  struct Input const start = mk_input(0U, BTN_A);
  app_tick(&a, &start, 0U, &fb);

  /* Hold LEFT and UP for many frames; object must clamp to >= 0. */
  struct Input const lu = mk_input(BTN_LEFT | BTN_UP, 0U);
  for (int i = 0; i < 200; ++i) {
    app_tick(&a, &lu, (uint32_t)i, &fb);
  }
  EXPECT_GE(app_obj_x(&a), 0);
  EXPECT_GE(app_obj_y(&a), 0);

  /* Hold RIGHT and DOWN; object must clamp within the screen. */
  struct Input const rd = mk_input(BTN_RIGHT | BTN_DOWN, 0U);
  for (int i = 0; i < 200; ++i) {
    app_tick(&a, &rd, (uint32_t)i, &fb);
  }
  EXPECT_LE(app_obj_x(&a), SCREEN_W);
  EXPECT_LE(app_obj_y(&a), SCREEN_H);
}

TEST(App, NoInputEmitsNoCue) {
  App a;
  app_init(&a, 1U);
  Framebuffer fb;
  struct Input const start = mk_input(0U, BTN_A);
  app_tick(&a, &start, 0U, &fb);
  (void)app_take_cue(&a);

  struct Input const idle = mk_input(0U, 0U);
  app_tick(&a, &idle, 33U, &fb);
  EXPECT_EQ(app_take_cue(&a), CUE_NONE);
}
