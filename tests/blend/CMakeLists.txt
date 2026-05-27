add_executable(blend_test blend_test.cc)
target_link_libraries(blend_test PRIVATE Renderer::Blend gtest_main)
gtest_discover_tests(blend_test PROPERTIES LABELS blend)
