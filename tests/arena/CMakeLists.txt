add_executable(arena_test arena_test.cc)
target_link_libraries(arena_test PRIVATE Renderer::Arena gtest_main)
gtest_discover_tests(arena_test PROPERTIES LABELS arena)
