add_executable(clip_test clip_test.cc)
target_link_libraries(clip_test PRIVATE Renderer::Clip gtest_main)
gtest_discover_tests(clip_test PROPERTIES LABELS clip)
