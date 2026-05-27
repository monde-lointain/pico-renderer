add_executable(aa_test aa_test.cc)
target_link_libraries(aa_test PRIVATE Renderer::Aa gtest_main)
gtest_discover_tests(aa_test PROPERTIES LABELS aa)
