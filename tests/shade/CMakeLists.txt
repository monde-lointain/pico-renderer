add_executable(shade_test shade_test.cc)
target_link_libraries(shade_test PRIVATE Renderer::Shade renderer::harness gtest_main)
gtest_discover_tests(shade_test PROPERTIES LABELS shade)
