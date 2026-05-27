/* main_pico.cc — Stream F.2 + G: PicoSystem bare-metal entry point.
 *
 * Owns the game loop; drives app_init / app_tick via platform_pico HAL.
 * plat_present is TE-gated (VSYNC), so it self-paces to ~30fps — no delay. */

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

int main(void) {
  plat_init();
  app_init(&s_app, plat_seed());

  for (;;) {
    struct Input in;
    plat_poll_input(&in); /* pico never returns false */
    app_tick(&s_app, &in, plat_millis(), &s_fb);

    /* Fire sound cue (non-blocking PWM beeper on GPIO11). */
    const enum Cue c = app_take_cue(&s_app);
    if (c != CUE_NONE) {
      plat_audio(cue_freq(c), cue_dur(c));
    }

    plat_present(&s_fb); /* TE-gated: blocks until VSYNC, self-pacing */
  }

  return 0; /* unreachable */
}
