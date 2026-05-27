/* platform_sdl.cc — SDL3 + ImGui host HAL
 *
 * Implements platform.h on SDL3 v3.4.8.
 * Orthodoxy-EXEMPT: SDL/ImGui are modern C++ — local idioms match the API.
 *
 * Window:   integer-scaled (1x–4x, default 3x) SDL_PIXELFORMAT_RGB565 texture.
 * Scaling:  SDL_SCALEMODE_NEAREST (SDL_surface.h line 90) for crisp pixels.
 * Input:    arrows → D-pad; Z/X/A/S → A/B/X/Y (SDL_scancode.h lines 63-88).
 * ImGui:    imgui_impl_sdl3 + imgui_impl_sdlrenderer3 backends.
 *           A "View" menu in the main menu bar lets the user pick 1x–4x scale.
 * Time:     SDL_GetTicks() — ms since init.
 * Seed:     time(nullptr) — wall clock.
 * Log:      vfprintf to stderr.
 */

#include <SDL3/SDL.h>
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"
#include "platform/platform.h"

/* ---- Module state -------------------------------------------------------- */
static SDL_Window *s_window = nullptr;
static SDL_Renderer *s_renderer = nullptr;
static SDL_Texture *s_texture =
    nullptr; /* RGB565 streaming, SCREEN_W×SCREEN_H */

static int s_scale = 3; /* current integer scale 1–4 */
static bool s_quit = false;

/* Input state for edge detection */
static uint32_t s_held_prev = 0;

/* ---- Key → Button mapping ------------------------------------------------ */
/* SDL_scancode.h:
 *   SDL_SCANCODE_UP=82, DOWN=81, LEFT=80, RIGHT=79
 *   SDL_SCANCODE_Z=29, X=27, A=4, S=22
 * Spec: arrows→D-pad; Z→A, X→B, A→X, S→Y */
static uint32_t scancode_to_btn(SDL_Scancode sc) {
  switch (sc) {
    case SDL_SCANCODE_UP:
      return BTN_UP;
    case SDL_SCANCODE_DOWN:
      return BTN_DOWN;
    case SDL_SCANCODE_LEFT:
      return BTN_LEFT;
    case SDL_SCANCODE_RIGHT:
      return BTN_RIGHT;
    case SDL_SCANCODE_Z:
      return BTN_A;
    case SDL_SCANCODE_X:
      return BTN_B;
    case SDL_SCANCODE_A:
      return BTN_X;
    case SDL_SCANCODE_S:
      return BTN_Y;
    default:
      return 0;
  }
}

/* ---- plat_init ----------------------------------------------------------- */
void plat_init(void) {
  SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS);

  /* Window size: scale*240 × scale*240 + room for menu bar (~20px) */
  const int win_w = SCREEN_W * s_scale;
  const int win_h = SCREEN_H * s_scale + 20;

  s_window = SDL_CreateWindow("PicoSystem Template", win_w, win_h,
                              SDL_WINDOW_RESIZABLE);

  /* SDL_CreateRenderer(window, name=NULL uses first available driver)
   * SDL_render.h line 273 */
  s_renderer = SDL_CreateRenderer(s_window, nullptr);

  /* Streaming RGB565 texture for the framebuffer
   * SDL_PIXELFORMAT_RGB565 = 0x15151002 (SDL_pixels.h line 591)
   * SDL_TEXTUREACCESS_STREAMING (SDL_render.h line 104) */
  s_texture =
      SDL_CreateTexture(s_renderer, SDL_PIXELFORMAT_RGB565,
                        SDL_TEXTUREACCESS_STREAMING, SCREEN_W, SCREEN_H);

  /* Nearest-neighbour scaling so integer upscale stays crisp
   * SDL_SCALEMODE_NEAREST (SDL_surface.h line 90) */
  SDL_SetTextureScaleMode(s_texture, SDL_SCALEMODE_NEAREST);

  /* ImGui setup */
  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr; /* no imgui.ini */

  /* imgui_impl_sdl3.h line 35 */
  ImGui_ImplSDL3_InitForSDLRenderer(s_window, s_renderer);
  /* imgui_impl_sdlrenderer3.h line 33 */
  ImGui_ImplSDLRenderer3_Init(s_renderer);
}

/* ---- plat_poll_input ----------------------------------------------------- */
bool plat_poll_input(struct Input *out) {
  uint32_t held = s_held_prev;

  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    /* Let ImGui handle its own events first */
    ImGui_ImplSDL3_ProcessEvent(&ev);

    if (ev.type == SDL_EVENT_QUIT) {
      s_quit = true;
    } else if (ev.type == SDL_EVENT_KEY_DOWN && !ev.key.repeat) {
      const uint32_t btn = scancode_to_btn(ev.key.scancode);
      held |= btn;
    } else if (ev.type == SDL_EVENT_KEY_UP) {
      const uint32_t btn = scancode_to_btn(ev.key.scancode);
      held &= ~btn;
    }
  }

  out->held = held;
  out->pressed = held & ~s_held_prev;
  s_held_prev = held;

  return !s_quit;
}

/* ---- plat_present -------------------------------------------------------- */
void plat_present(const struct Framebuffer *fb) {
  /* Upload framebuffer to streaming texture.
   * pitch = SCREEN_W * 2 (2 bytes per RGB565 pixel)
   * SDL_render.h line 1324 */
  SDL_UpdateTexture(s_texture, nullptr, fb->px,
                    SCREEN_W * (int)sizeof(color_t));

  /* --- ImGui frame --- */
  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();

  /* Main menu bar: View → scale selection */
  if (ImGui::BeginMainMenuBar()) {
    if (ImGui::BeginMenu("View")) {
      for (int s = 1; s <= 4; ++s) {
        char label[8];
        SDL_snprintf(label, sizeof label, "%dx", s);
        const bool sel = (s_scale == s);
        if (ImGui::MenuItem(label, nullptr, sel)) {
          s_scale = s;
          SDL_SetWindowSize(s_window, SCREEN_W * s_scale,
                            SCREEN_H * s_scale + 20);
        }
      }
      ImGui::EndMenu();
    }
    ImGui::EndMainMenuBar();
  }

  ImGui::Render();

  /* --- Render --- */
  SDL_SetRenderDrawColor(s_renderer, 0, 0, 0, 255);
  SDL_RenderClear(s_renderer);

  /* Get current render output size (may differ from window size on HiDPI) */
  int out_w = 0;
  int out_h = 0;
  SDL_GetRenderOutputSize(s_renderer, &out_w, &out_h);

  /* Integer-scaled destination rect, centered */
  const int fb_w = SCREEN_W * s_scale;
  const int fb_h = SCREEN_H * s_scale;
  const int off_x = (out_w - fb_w) / 2;
  /* Leave room for menu bar (~20px at logical coords, but we just push down) */
  const int off_y = out_h - fb_h - (out_h - fb_h) / 2;

  SDL_FRect dst;
  dst.x = (float)off_x;
  dst.y = (float)off_y;
  dst.w = (float)fb_w;
  dst.h = (float)fb_h;

  SDL_RenderTexture(s_renderer, s_texture, nullptr, &dst);

  /* Draw ImGui on top */
  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), s_renderer);

  SDL_RenderPresent(s_renderer);
}

/* ---- plat_millis --------------------------------------------------------- */
uint32_t plat_millis(void) {
  /* SDL_GetTicks() returns ms since SDL_Init (SDL3 API) */
  return (uint32_t)SDL_GetTicks();
}

/* ---- plat_seed ----------------------------------------------------------- */
uint32_t plat_seed(void) { return (uint32_t)time(nullptr); }

/* ---- plat_log ------------------------------------------------------------ */
void plat_log(const char *fmt, ...) {
  va_list ap;
  va_start(ap, fmt);
  /* False positive: the analyzer doesn't model va_start initializing ap here.
   */
  /* NOLINTNEXTLINE(clang-analyzer-valist.Uninitialized) */
  vfprintf(stderr, fmt, ap);
  va_end(ap);
}

/* ---- plat_audio ---------------------------------------------------------- */
/* Documented no-op: host sound is non-essential.  SDL3 audio API differs
 * enough across versions that a portable square-wave synthesiser adds
 * significant complexity for negligible value on the host target.
 * The pico HAL provides the real beeper (GPIO11 PWM). */
void plat_audio(uint32_t freq_hz, uint32_t duration_ms) {
  (void)freq_hz;
  (void)duration_ms;
}
