#ifndef PLATFORM_H
#define PLATFORM_H

#include <stdint.h>

#include "gfx/framebuffer.h"

/* FROZEN CONTRACT (Stream A). Thin HAL, link-time substituted per target
 * (platform_sdl.cc / platform_pico.cc / FakePlatform in tests). */

/* Button bitmask. Console maps GPIO; PC maps keys (arrows + Z/X/A/S). */
enum Button {
  BTN_UP = 1U << 0,
  BTN_DOWN = 1U << 1,
  BTN_LEFT = 1U << 2,
  BTN_RIGHT = 1U << 3,
  BTN_A = 1U << 4,
  BTN_B = 1U << 5,
  BTN_X = 1U << 6,
  BTN_Y = 1U << 7
};

struct Input {
  uint32_t held;    /* buttons currently down */
  uint32_t pressed; /* buttons that went down this frame (edge) */
};

void plat_init(void);

/* Fill *out with this frame's input. Returns false if the platform requests
 * shutdown (e.g. PC window closed); true otherwise. */
bool plat_poll_input(struct Input *out);

/* Present a finished framebuffer to the display. */
void plat_present(const struct Framebuffer *fb);

/* Milliseconds since init (monotonic). */
uint32_t plat_millis(void);

/* Entropy for seeding the shuffle RNG. */
uint32_t plat_seed(void);

/* printf-style logging: host -> stdout, console -> USB-CDC. */
void plat_log(const char *fmt, ...);

/* Fire-and-forget beeper cue. freq_hz==0 is silence. Non-blocking: starts the
 * tone and returns; the HAL stops it after duration_ms. Console = PWM beeper
 * (GPIO11); host may stub or synthesize. (Contract amended post-Wave-2 for the
 * sound polish step, as the plan anticipated.) */
void plat_audio(uint32_t freq_hz, uint32_t duration_ms);

#endif /* PLATFORM_H */
