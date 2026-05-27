/* main_sdl.cc — Stream E.2 + G: SDL host entry point.
 *
 * Owns the 30fps game loop; drives app_init / app_tick via platform_sdl HAL.
 * The App is pure: it never calls plat_*. */

#include <SDL3/SDL.h>
#include <stdint.h>

#include "app/app.h"
#include "gfx/framebuffer.h"
#include "platform/platform.h"

/* Static allocation: Framebuffer is ~112 KB — keep off the stack. */
static App s_app;
static Framebuffer s_fb;

/* ---- Cue -> (freq_hz, duration_ms) mapping ---- */
static uint32_t cue_freq(enum Cue c) {
  switch (c) {
    case CUE_SELECT:
      return 660U;
    case CUE_MOVE:
      return 330U;
    default:
      return 0U;
  }
}

static uint32_t cue_dur(enum Cue c) {
  switch (c) {
    case CUE_SELECT:
      return 40U;
    case CUE_MOVE:
      return 20U;
    default:
      return 0U;
  }
}

int main(int argc, char **argv) {
  (void)argc;
  (void)argv;

  plat_init();
  app_init(&s_app, plat_seed());

  uint32_t frame_start = plat_millis();

  for (;;) {
    struct Input in;
    if (!plat_poll_input(&in)) {
      break;
    }

    app_tick(&s_app, &in, plat_millis(), &s_fb);

    /* Fire sound cue (non-blocking; SDL impl is a documented no-op). */
    const enum Cue c = app_take_cue(&s_app);
    if (c != CUE_NONE) {
      plat_audio(cue_freq(c), cue_dur(c));
    }

    plat_present(&s_fb);

    /* Pace to ~30fps: target 33ms per frame. */
    const uint32_t elapsed = plat_millis() - frame_start;
    if (elapsed < 33U) {
      SDL_Delay(33U - elapsed);
    }
    frame_start = plat_millis();
  }

  return 0;
}
