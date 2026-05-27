add_executable(rdr_test rdr_test.cc)
target_link_libraries(rdr_test PRIVATE Renderer::rdr gtest_main)
gtest_discover_tests(rdr_test PROPERTIES LABELS rdr)
