# Your game logic lives here. Ships only the seedable PRNG; add your own modules.
add_library(game STATIC
    rng.cc
)
add_library(renderer::game ALIAS game)
target_include_directories(game PUBLIC ${PROJECT_SOURCE_DIR}/src)
target_link_libraries(game PRIVATE project_warnings)
# PUBLIC so test binaries linking this lib inherit -fsanitize (no-op when off).
target_link_libraries(game PUBLIC renderer::sanitizers)
orthodoxy_enforce(game)
pico_tidy_enable(game)
