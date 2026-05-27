include(GoogleTest)

add_executable(game_test
    rng_test.cc
)
target_link_libraries(game_test PRIVATE renderer::game gtest_main)
gtest_discover_tests(game_test)
