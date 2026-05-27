add_executable(tex_test tex_test.cc)
target_link_libraries(tex_test PRIVATE Renderer::Tex gtest_main)
gtest_discover_tests(tex_test PROPERTIES LABELS tex)
