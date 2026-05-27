# Demo app (integrator-owned). Host-only stub at Stream A; device demo wired at C1.
# NOT orthodoxy_enforce'd (app-level, like src/app) — ImGui/demo carve-out.
if(PLATFORM STREQUAL "sdl")
  add_executable(renderer_demo main.cc)
  target_link_libraries(renderer_demo PRIVATE Renderer::rdr project_warnings)
  target_include_directories(renderer_demo PRIVATE ${CMAKE_SOURCE_DIR}/src)
endif()
