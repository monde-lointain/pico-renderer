# Presenter / app subsystem: your game's state machine + rendering.
add_library(app STATIC
    app.cc
)
add_library(renderer::app ALIAS app)
target_include_directories(app PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(app
    # sanitizers PUBLIC so test binaries linking app inherit -fsanitize (no-op off).
    PUBLIC  renderer::game renderer::gfx renderer::sanitizers
    PRIVATE project_warnings
)
orthodoxy_enforce(app)
pico_tidy_enable(app)
