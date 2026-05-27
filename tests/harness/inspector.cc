// inspector.cc — ImGui frame inspector. See inspector.h.
// Orthodoxy-EXEMPT (ImGui/SDL carve-out); local idioms match the SDL3/ImGui API
// exactly as platform_sdl.cc does. Verified against the fetched SDL3 v3.4.8 +
// ImGui v1.92.8 sources (same calls platform_sdl.cc uses).

#include "inspector.h"

#include <SDL3/SDL.h>
#include <stdlib.h>

#include "imgui.h"
#include "imgui_impl_sdl3.h"
#include "imgui_impl_sdlrenderer3.h"

struct Inspector {
  SDL_Window* window;
  SDL_Renderer* renderer;
  SDL_Texture* texture;  // RGB565 streaming, width x height
  int width;
  int height;
  int scale;
  bool quit;
};

struct Inspector* inspector_create(const char* title, int width, int height) {
  if (width <= 0 || height <= 0) return nullptr;
  if (!SDL_Init(SDL_INIT_VIDEO | SDL_INIT_EVENTS)) return nullptr;

  struct Inspector* insp =
      (struct Inspector*)calloc(1, sizeof(struct Inspector));
  if (!insp) return nullptr;
  insp->width = width;
  insp->height = height;
  insp->scale = 2;
  insp->quit = false;

  const int win_w = width * insp->scale + 220;  // room for the toggle panel
  const int win_h = height * insp->scale + 20;
  insp->window = SDL_CreateWindow(title ? title : "Frame Inspector", win_w,
                                  win_h, SDL_WINDOW_RESIZABLE);
  if (!insp->window) {
    free(insp);
    return nullptr;
  }

  insp->renderer = SDL_CreateRenderer(insp->window, nullptr);
  if (!insp->renderer) {
    SDL_DestroyWindow(insp->window);
    free(insp);
    return nullptr;
  }

  insp->texture = SDL_CreateTexture(insp->renderer, SDL_PIXELFORMAT_RGB565,
                                    SDL_TEXTUREACCESS_STREAMING, width, height);
  SDL_SetTextureScaleMode(insp->texture, SDL_SCALEMODE_NEAREST);

  ImGui::CreateContext();
  ImGui::GetIO().IniFilename = nullptr;
  ImGui_ImplSDL3_InitForSDLRenderer(insp->window, insp->renderer);
  ImGui_ImplSDLRenderer3_Init(insp->renderer);
  return insp;
}

void inspector_destroy(struct Inspector* insp) {
  if (!insp) return;
  ImGui_ImplSDLRenderer3_Shutdown();
  ImGui_ImplSDL3_Shutdown();
  ImGui::DestroyContext();
  if (insp->texture) SDL_DestroyTexture(insp->texture);
  if (insp->renderer) SDL_DestroyRenderer(insp->renderer);
  if (insp->window) SDL_DestroyWindow(insp->window);
  free(insp);
}

int inspector_frame(struct Inspector* insp, const uint16_t* fb565,
                    struct InspectorToggles* toggles) {
  if (!insp) return 1;

  SDL_Event ev;
  while (SDL_PollEvent(&ev)) {
    ImGui_ImplSDL3_ProcessEvent(&ev);
    if (ev.type == SDL_EVENT_QUIT) insp->quit = true;
  }

  if (fb565) {
    SDL_UpdateTexture(insp->texture, nullptr, fb565,
                      insp->width * (int)sizeof(uint16_t));
  }

  ImGui_ImplSDLRenderer3_NewFrame();
  ImGui_ImplSDL3_NewFrame();
  ImGui::NewFrame();

  ImGui::Begin("Stages");
  if (toggles) {
    ImGui::Checkbox("Transform overlay", (bool*)&toggles->show_transform);
    ImGui::Checkbox("Raster framebuffer", (bool*)&toggles->show_raster);
    ImGui::Checkbox("Tile grid", (bool*)&toggles->show_grid);
  }
  ImGui::End();

  ImGui::Render();

  SDL_SetRenderDrawColor(insp->renderer, 0, 0, 0, 255);
  SDL_RenderClear(insp->renderer);

  bool draw_fb = !toggles || toggles->show_raster;
  if (draw_fb) {
    SDL_FRect dst;
    dst.x = 0.0f;
    dst.y = 20.0f;
    dst.w = (float)(insp->width * insp->scale);
    dst.h = (float)(insp->height * insp->scale);
    SDL_RenderTexture(insp->renderer, insp->texture, nullptr, &dst);
  }

  ImGui_ImplSDLRenderer3_RenderDrawData(ImGui::GetDrawData(), insp->renderer);
  SDL_RenderPresent(insp->renderer);

  return insp->quit ? 1 : 0;
}
