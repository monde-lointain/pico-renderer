/* assets_test.cc — acceptance tests for gen_assets output.
 * The template ships one asset: the font atlas. */

#include <gtest/gtest.h>
#include <stdint.h>

#include "gfx/assets_gen.h"
#include "gfx/sprite.h"

/* ---- sprite count -------------------------------------------------------- */

TEST(AssetsGen, EmitsExpectedSpriteCount) {
  /* font atlas = 1 sprite ID. */
  EXPECT_EQ((int)SPRITE_ID_COUNT, 1);
}

/* ---- dimension checks ---------------------------------------------------- */

TEST(AssetsGen, FontAtlasIs208x66) {
  const Sprite *s = sprite_get(SPRITE_FONT);
  ASSERT_NE(s, (const Sprite *)0);
  EXPECT_EQ(s->w, 208);
  EXPECT_EQ(s->h, 66);
}

/* ---- all sprite IDs return non-null ------------------------------------- */

TEST(AssetsGen, AllSpriteIdsReturnNonNull) {
  int id;
  for (id = 0; id < (int)SPRITE_ID_COUNT; ++id) {
    const Sprite *s = sprite_get((SpriteId)id);
    EXPECT_NE(s, (const Sprite *)0) << "sprite_get failed for id=" << id;
  }
}
