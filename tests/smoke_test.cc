#include <gtest/gtest.h>

#include "app/app.h"
#include "gfx/assets_gen.h"
#include "gfx/framebuffer.h"
#include "gfx/sprite.h"
#include "platform/platform.h"

/* Engine smoke test: the contracts a new game builds on. */

TEST(FramebufferContract, Rgb565PacksRedInHighBits) {
  EXPECT_EQ(rgb565(0xFF, 0x00, 0x00), 0xF800);
  EXPECT_EQ(rgb565(0x00, 0xFF, 0x00), 0x07E0);
  EXPECT_EQ(rgb565(0x00, 0x00, 0xFF), 0x001F);
  EXPECT_EQ(sizeof(Framebuffer), (size_t)(SCREEN_PIXELS * 2));
}

TEST(AssetsContract, FontSpriteIsAvailable) {
  EXPECT_NE(sprite_get(SPRITE_FONT), (const struct Sprite *)0);
}

TEST(AppContract, InitThenTickRunsWithoutPlatform) {
  App a;
  app_init(&a, 1u);
  EXPECT_EQ(app_state(&a), APP_TITLE);

  Framebuffer fb;
  struct Input in;
  in.held = 0u;
  in.pressed = BTN_A;
  app_tick(&a, &in, 0u, &fb);
  EXPECT_EQ(app_state(&a), APP_PLAYING);
}
