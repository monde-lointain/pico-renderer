add_executable(cmd_test cmd_test.cc)
target_link_libraries(cmd_test PRIVATE Renderer::Cmd gtest_main)
gtest_discover_tests(cmd_test PROPERTIES LABELS cmd)
