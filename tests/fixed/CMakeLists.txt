add_executable(fixed_test fixed_test.cc)
target_link_libraries(fixed_test PRIVATE Renderer::Fixed gtest_main)
gtest_discover_tests(fixed_test PROPERTIES LABELS fixed)
